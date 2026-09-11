#include "pages/sensors/sensors_model.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unistd.h>

namespace sensors {
namespace {

std::string csv_text(const std::string &value)
{
    std::string result = "\"";
    for (const char ch : value) {
        if (ch == '"') result += '"';
        result += ch;
    }
    return result + '"';
}

std::string utc_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
         << (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000)
         << 'Z';
    return text.str();
}

std::array<std::string, 4> recording_channel_key(const SensorReading &reading, const ChannelReading &channel)
{
    return {reading.device_id, reading.id, channel.name, channel.unit};
}

}  // namespace

void SensorsModel::ingest(Snapshot snapshot)
{
    if (paused_) return;
    const bool new_sample = snapshot.sequence != 0 && snapshot.sequence != last_sequence_;
    snapshot_ = std::move(snapshot);
    if (filter_ >= 0 && std::none_of(snapshot_.buses.begin(), snapshot_.buses.end(), [this](const auto &bus) {
            return bus.number == filter_;
        })) filter_ = -1;
    for (auto &sensor : snapshot_.sensors) {
        sensor.device_id = sensor_device_id(sensor);
        for (const auto &channel : sensor.channels) {
            if (sensor.available && !std::isfinite(channel.value)) {
                sensor.available = false;
                sensor.error = "Invalid sensor value";
            }
        }
    }
    const auto visible = visible_devices();
    const bool selection_visible = std::any_of(visible.begin(), visible.end(), [this](const auto &device) {
        return device.id == selected_id_;
    });
    if (!selection_visible) selected_id_ = visible.empty() ? "" : visible.front().id;
    if (!new_sample) return;
    last_sequence_ = snapshot_.sequence;
    if (origin_ == Clock::time_point{}) origin_ = snapshot_.timestamp;
    const double seconds = std::chrono::duration<double>(snapshot_.timestamp - origin_).count();
    const std::string wall_time = recording() ? utc_timestamp() : "";
    for (const auto &sensor : snapshot_.sensors) {
        auto &history = histories_[sensor.id];
        const auto count = sensor.channels.size();
        if (history.minimum.size() != count) {
            history = {};
            history.minimum.assign(count, std::numeric_limits<double>::infinity());
            history.maximum.assign(count, -std::numeric_limits<double>::infinity());
        }
        SamplePoint point{seconds, {}};
        for (std::size_t i = 0; i < count; ++i) {
            const auto &channel = sensor.channels[i];
            const double value = sensor.available ? channel.value : std::numeric_limits<double>::quiet_NaN();
            point.values.push_back(value);
            if (sensor.available) {
                history.minimum[i] = std::min(history.minimum[i], value);
                history.maximum[i] = std::max(history.maximum[i], value);
            }
            if (recording() && disabled_recording_channels_.count(recording_channel_key(sensor, channel)) == 0) {
                recording_ << csv_text(wall_time) << ',' << std::setprecision(12) << seconds << ','
                           << csv_text(sensor.id) << ',' << csv_text(sensor.device) << ','
                           << csv_text(sensor.location) << ',' << csv_text(channel.name) << ',';
                if (sensor.available) recording_ << std::setprecision(12) << value;
                recording_ << ',' << csv_text(channel.unit) << ',' << (sensor.available ? "ok" : "error")
                           << ',' << csv_text(sensor.error) << ',' << (snapshot_.simulated ? "true" : "false")
                           << ',' << csv_text(sensor.device_id) << '\n';
                ++recorded_rows_;
            }
        }
        history.points.push_back(std::move(point));
        while (history.points.size() > 300) history.points.pop_front();
    }
    // Remove histories of detached devices so repeated hotplug cannot grow memory forever.
    for (auto it = histories_.begin(); it != histories_.end();) {
        const bool present = std::any_of(snapshot_.sensors.begin(), snapshot_.sensors.end(), [&](const auto &s) {
            return s.id == it->first;
        });
        if (present) ++it;
        else it = histories_.erase(it);
    }
    if (recording()) {
        recording_.flush();
        if (!recording_) {
            recording_error_ = "Cannot write recording: " + recording_path_;
            recording_.close();
        }
    }
}

std::vector<const SensorReading *> SensorsModel::visible_sensors() const
{
    std::vector<const SensorReading *> result;
    for (const auto &sensor : snapshot_.sensors) {
        if (filter_ == -1 || (filter_ == -2 && sensor.location == "system") ||
            (filter_ >= 0 && sensor.location == "i2c-" + std::to_string(filter_))) result.push_back(&sensor);
    }
    return result;
}

std::vector<SensorDevice> SensorsModel::visible_devices() const
{
    std::vector<SensorDevice> result;
    std::map<std::string, std::size_t> indices;
    for (const auto *sensor : visible_sensors()) {
        const auto [found, inserted] = indices.emplace(sensor->device_id, result.size());
        if (inserted) {
            const auto path = std::filesystem::path(sensor->device_id).is_absolute()
                                  ? sensor->device_id : sensor->path;
            result.push_back({sensor->device_id, sensor->device.empty() ? sensor->name : sensor->device,
                              sensor->location, path, {}});
        }
        result[found->second].readings.push_back(sensor);
    }
    return result;
}

void SensorsModel::select(const std::string &id)
{
    for (const auto &device : visible_devices()) {
        if (device.id == id) {
            selected_id_ = id;
            return;
        }
    }
}

void SensorsModel::set_filter(int bus)
{
    filter_ = bus;
    const auto visible = visible_devices();
    if (std::none_of(visible.begin(), visible.end(), [this](const auto &device) { return device.id == selected_id_; })) {
        selected_id_ = visible.empty() ? "" : visible.front().id;
    }
}

void SensorsModel::set_paused(bool paused)
{
    paused_ = paused;
}

void SensorsModel::clear_history()
{
    histories_.clear();
}

bool SensorsModel::channel_recording_enabled(const std::string &reading_id, std::size_t channel_index) const
{
    const auto found = std::find_if(snapshot_.sensors.begin(), snapshot_.sensors.end(), [&](const auto &reading) {
        return reading.id == reading_id;
    });
    return found != snapshot_.sensors.end() && channel_index < found->channels.size() &&
           disabled_recording_channels_.count(recording_channel_key(*found, found->channels[channel_index])) == 0;
}

void SensorsModel::set_channel_recording_enabled(const std::string &reading_id, std::size_t channel_index, bool enabled)
{
    const auto found = std::find_if(snapshot_.sensors.begin(), snapshot_.sensors.end(), [&](const auto &reading) {
        return reading.id == reading_id;
    });
    if (found == snapshot_.sensors.end() || channel_index >= found->channels.size()) return;
    const auto key = recording_channel_key(*found, found->channels[channel_index]);
    if (enabled) disabled_recording_channels_.erase(key);
    else disabled_recording_channels_.insert(key);
}

std::size_t SensorsModel::recording_channel_count() const
{
    std::size_t count = 0;
    for (const auto &reading : snapshot_.sensors) {
        for (const auto &channel : reading.channels) {
            if (disabled_recording_channels_.count(recording_channel_key(reading, channel)) == 0) ++count;
        }
    }
    return count;
}

const SensorReading *SensorsModel::selected() const
{
    for (const auto &sensor : snapshot_.sensors) if (sensor.device_id == selected_id_) return &sensor;
    return nullptr;
}

SensorDevice SensorsModel::selected_device() const
{
    for (const auto &device : visible_devices()) if (device.id == selected_id_) return device;
    return {};
}

const SensorHistory *SensorsModel::history() const
{
    const auto *reading = selected();
    return reading ? history(reading->id) : nullptr;
}

const SensorHistory *SensorsModel::history(const std::string &reading_id) const
{
    const auto found = histories_.find(reading_id);
    return found == histories_.end() ? nullptr : &found->second;
}

bool SensorsModel::start_recording(const std::filesystem::path &directory)
{
    if (recording()) return true;
    recording_error_.clear();
    recorded_rows_ = 0;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        recording_error_ = "Cannot create recording directory: " + ec.message();
        return false;
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    unsigned suffix = 0;
    std::filesystem::path path;
    do {
        path = directory / ("sensors-" + std::to_string(milliseconds) + "-" + std::to_string(getpid()) + "-" +
                            std::to_string(suffix++) + ".csv");
    } while (std::filesystem::exists(path, ec) && !ec);
    recording_.clear();
    recording_.imbue(std::locale::classic());
    recording_.open(path);
    if (!recording_) {
        recording_error_ = "Cannot open recording: " + path.string();
        return false;
    }
    recording_path_ = path.string();
    recording_ << "timestamp_utc,elapsed_s,sensor_id,device,bus,channel,value,unit,status,error,simulated,device_id\n";
    recording_.flush();
    if (!recording_) {
        recording_error_ = "Cannot write recording: " + recording_path_;
        recording_.close();
        return false;
    }
    return true;
}

void SensorsModel::stop_recording()
{
    if (!recording()) return;
    recording_.flush();
    if (!recording_) recording_error_ = "Cannot finish recording: " + recording_path_;
    recording_.close();
}

}  // namespace sensors
