#include "pages/sensors/sensors_view_model.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace sensors {

SensorsViewModel::SensorsViewModel(std::unique_ptr<SensorSource> source) : source_(std::move(source))
{
    if (source_) source_->set_interval(interval_);
}

void SensorsViewModel::publish()
{
    revision_.set(revision_.value() == std::numeric_limits<int>::max() ? 0 : revision_.value() + 1);
}

lv_subject_t *SensorsViewModel::revision_subject()
{
    return revision_.get();
}

void SensorsViewModel::tick()
{
    if (!source_ || model_.paused()) return;
    auto snapshot = source_->snapshot();
    if (snapshot.timestamp != Clock::time_point{} &&
        Clock::now() - snapshot.timestamp > std::chrono::milliseconds(std::max(2000u, interval_ * 3))) {
        for (auto &sensor : snapshot.sensors) {
            sensor.available = false;
            sensor.error = "Sensor data is stale";
        }
    }
    model_.ingest(std::move(snapshot));
    reconcile_detail();
    publish();
}

void SensorsViewModel::select(const std::string &id)
{
    model_.select(id);
    reconcile_detail();
    publish();
}

void SensorsViewModel::open_sensor(const std::string &id)
{
    const auto visible = model_.visible_devices();
    if (std::none_of(visible.begin(), visible.end(), [&](const auto &device) { return device.id == id; })) return;
    model_.select(id);
    detail_id_ = id;
    publish();
}

void SensorsViewModel::show_list()
{
    detail_id_.clear();
    publish();
}

void SensorsViewModel::reconcile_detail()
{
    if (detail_id_ != model_.selected_id()) detail_id_.clear();
}

void SensorsViewModel::set_filter(int bus)
{
    model_.set_filter(bus);
    reconcile_detail();
    publish();
}

void SensorsViewModel::toggle_pause()
{
    model_.set_paused(!model_.paused());
    if (!model_.paused()) tick();
    else publish();
}

void SensorsViewModel::clear_history()
{
    model_.clear_history();
    publish();
}

void SensorsViewModel::rescan()
{
    if (source_) source_->rescan();
    if (model_.paused()) model_.set_paused(false);
    tick();
}

void SensorsViewModel::set_interval(unsigned milliseconds)
{
    if (milliseconds != 100 && milliseconds != 250 && milliseconds != 1000) return;
    interval_ = milliseconds;
    if (source_) source_->set_interval(interval_);
    publish();
}

void SensorsViewModel::toggle_recording()
{
    if (model_.recording()) model_.stop_recording();
    else {
        std::filesystem::path directory;
        if (const auto *value = std::getenv("LILYGO_SENSORS_RECORD_DIR"); value && *value) directory = value;
        else if (const auto *value = std::getenv("XDG_DATA_HOME"); value && *value)
            directory = std::filesystem::path(value) / "lilygo-ui-sensors" / "recordings";
        else if (const auto *value = std::getenv("HOME"); value && *value)
            directory = std::filesystem::path(value) / ".local/share/lilygo-ui-sensors/recordings";
        else directory = std::filesystem::temp_directory_path() / "lilygo-ui-sensors-recordings";
        model_.start_recording(directory);
    }
    publish();
}

void SensorsViewModel::set_channel_recording_enabled(const std::string &reading_id, std::size_t channel_index, bool enabled)
{
    model_.set_channel_recording_enabled(reading_id, channel_index, enabled);
    publish();
}

}  // namespace sensors
