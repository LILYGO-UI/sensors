#include "pages/sensors/sensors_model.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <unistd.h>

using namespace sensors;

namespace {
Snapshot fixture(unsigned long long sequence, double x = 1.0)
{
    Snapshot snapshot;
    snapshot.sequence = sequence;
    snapshot.timestamp = Clock::now();
    snapshot.buses = {{1, "I2C 1", "", {}}, {2, "I2C 2", "", {}}};
    snapshot.sensors = {
        {"imu/accel", "Accelerometer", "accelerometer", "qmi8658c", "i2c-1", "/fixture/imu", "", true,
         {{"X", "m/s2", x}, {"Y", "m/s2", -2}, {"Z", "m/s2", 9.8}}, snapshot.timestamp, {}, {}},
        {"cpu/temp", "Temperature", "temperature", "cpu_thermal", "system", "/fixture/cpu", "", true,
         {{"Temperature", "C", 42}}, snapshot.timestamp, {}, {}},
    };
    return snapshot;
}

Snapshot motion_fixture(unsigned long long sequence, double x = 1.0)
{
    auto data = fixture(sequence, x);
    auto gyro = data.sensors.front();
    gyro.id = "imu/gyro";
    gyro.name = "Gyroscope";
    gyro.category = "gyroscope";
    gyro.channels = {{"X", "rad/s", 0.5}, {"Y", "rad/s", -0.2}, {"Z", "rad/s", 0.1}};
    data.sensors.push_back(gyro);
    return data;
}

std::vector<std::string> recording_lines(const std::string &path)
{
    std::ifstream recording(path);
    assert(recording.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(recording, line)) lines.push_back(line);
    return lines;
}

void selected_channel_recording()
{
    SensorsModel model;
    assert(model.recording_channel_count() == 0);
    assert(!model.channel_recording_enabled("missing", 0));
    model.ingest(motion_fixture(1));
    assert(model.recording_channel_count() == 7);
    for (const auto &reading : model.snapshot().sensors) {
        for (std::size_t i = 0; i < reading.channels.size(); ++i) {
            assert(model.channel_recording_enabled(reading.id, i));
            model.set_channel_recording_enabled(reading.id, i, false);
        }
    }
    model.set_channel_recording_enabled("imu/accel", 0, true);
    model.set_channel_recording_enabled("imu/gyro", 0, true);
    model.set_channel_recording_enabled("missing", 0, false);
    model.set_channel_recording_enabled("imu/accel", 3, false);
    assert(!model.channel_recording_enabled("imu/accel", 3));
    assert(model.recording_channel_count() == 2);

    const auto directory = std::filesystem::temp_directory_path() /
                           ("sensors-channel-test-" + std::to_string(getpid()));
    assert(model.start_recording(directory));
    const auto path = model.recording_path();
    model.ingest(motion_fixture(2, 2.125));
    assert(model.recorded_rows() == 2);
    model.set_channel_recording_enabled("imu/accel", 0, false);
    model.set_channel_recording_enabled("imu/gyro", 0, false);
    auto broken = motion_fixture(3);
    for (auto &reading : broken.sensors) {
        reading.available = false;
        reading.error = "Input/output error";
    }
    model.ingest(broken);
    assert(model.recording_channel_count() == 0 && model.recorded_rows() == 2);
    model.set_channel_recording_enabled("imu/accel", 1, true);
    ++broken.sequence;
    model.ingest(broken);
    assert(model.recorded_rows() == 3);
    model.set_channel_recording_enabled("imu/accel", 0, true);
    assert(model.recorded_rows() == 3 && model.recording_path() == path);
    model.ingest(motion_fixture(5, 4.25));
    assert(model.recorded_rows() == 5);
    assert(model.history("imu/accel")->points.size() == 5);
    assert(model.history("imu/gyro")->points.size() == 5);
    assert(model.history("cpu/temp")->points.size() == 5);
    assert(model.history("imu/accel")->points.back().values.size() == 3);
    assert(model.history("imu/accel")->points.back().values[2] == 9.8);
    model.stop_recording();
    const auto lines = recording_lines(path);
    assert(lines.size() == 6);
    assert(lines[1].find("\"imu/accel\",\"qmi8658c\",\"i2c-1\",\"X\",2.125,\"m/s2\",ok") != std::string::npos);
    assert(lines[2].find("\"imu/gyro\",\"qmi8658c\",\"i2c-1\",\"X\",0.5,\"rad/s\",ok") != std::string::npos);
    assert(lines[3].find("\"imu/accel\",\"qmi8658c\",\"i2c-1\",\"Y\",,\"m/s2\",error,\"Input/output error\"") != std::string::npos);
    assert(lines[4].find("\"X\",4.25,\"m/s2\",ok") != std::string::npos);
    assert(lines[5].find("\"Y\",-2,\"m/s2\",ok") != std::string::npos);
    for (std::size_t i = 1; i < lines.size(); ++i) {
        assert(lines[i].find("\"Z\"") == std::string::npos);
        assert(lines[i].find("\"cpu/temp\"") == std::string::npos);
    }

    model.set_channel_recording_enabled("imu/accel", 0, false);
    model.set_channel_recording_enabled("imu/accel", 1, false);
    assert(model.start_recording(directory));
    model.ingest(motion_fixture(6));
    model.stop_recording();
    assert(model.recorded_rows() == 0);
    assert(recording_lines(model.recording_path()).size() == 1);
    std::filesystem::remove_all(directory);
}

void recording_selection_identity()
{
    SensorsModel model;
    auto data = motion_fixture(1);
    model.ingest(data);
    model.set_channel_recording_enabled("imu/accel", 0, false);
    assert(model.channel_recording_enabled("imu/gyro", 0));
    assert(model.channel_recording_enabled("imu/accel", 1));
    model.set_filter(-2);
    model.select("/fixture/cpu");
    model.clear_history();
    assert(model.recording_channel_count() == 6);
    model.set_filter(-1);
    assert(!model.channel_recording_enabled("imu/accel", 0));
    std::reverse(data.sensors.front().channels.begin(), data.sensors.front().channels.end());
    std::reverse(data.sensors.begin(), data.sensors.end());
    ++data.sequence;
    model.ingest(data);
    assert(model.channel_recording_enabled("imu/accel", 0));
    assert(!model.channel_recording_enabled("imu/accel", 2));
    assert(model.channel_recording_enabled("imu/gyro", 0));
    data.sensors.back().channels[2].unit = "g";
    ++data.sequence;
    model.ingest(data);
    assert(model.channel_recording_enabled("imu/accel", 2));
    data.sensors.back().channels[2].unit = "m/s2";
    data.sensors.back().channels[2].name = "New X";
    ++data.sequence;
    model.ingest(data);
    assert(model.channel_recording_enabled("imu/accel", 2));

    auto detached = motion_fixture(5);
    detached.sensors.clear();
    model.ingest(detached);
    assert(model.recording_channel_count() == 0);
    assert(!model.channel_recording_enabled("imu/accel", 0));
    model.set_channel_recording_enabled("imu/accel", 0, true);
    model.ingest(motion_fixture(6));
    assert(!model.channel_recording_enabled("imu/accel", 0));
    assert(model.channel_recording_enabled("imu/accel", 1));

    auto replacement = motion_fixture(7);
    replacement.sensors.front().device_id = "different-physical-device";
    model.ingest(replacement);
    assert(model.channel_recording_enabled("imu/accel", 0));
    model.ingest(motion_fixture(8));
    assert(!model.channel_recording_enabled("imu/accel", 0));
}

void physical_device_grouping()
{
    SensorsModel model;
    auto data = fixture(1);
    auto gyro = data.sensors.front();
    gyro.id = "imu/gyro";
    gyro.name = "Gyroscope";
    gyro.category = "gyroscope";
    gyro.channels = {{"X", "rad/s", 0.5}, {"Y", "rad/s", -0.2}, {"Z", "rad/s", 0.1}};
    data.sensors.push_back(gyro);
    auto second = data.sensors.front();
    second.id = "second/accel";
    second.path = "/fixture/second";
    second.location = "i2c-2";
    data.sensors.push_back(second);
    model.ingest(data);
    assert(model.visible_sensors().size() == 4);
    assert(model.visible_devices().size() == 3);
    auto selected = model.selected_device();
    assert(selected.id == "/fixture/imu" && selected.name == "qmi8658c");
    assert(selected.location == "i2c-1" && selected.path == "/fixture/imu");
    assert(selected.readings.size() == 2);
    assert(selected.readings[0]->id == "imu/accel" && selected.readings[1]->id == "imu/gyro");
    assert(model.history("imu/accel")->maximum[0] == 1);
    assert(model.history("imu/gyro")->maximum[0] == 0.5);
    assert(model.history("missing") == nullptr);
    model.select("/fixture/second");
    assert(model.selected_device().readings.size() == 1);
    model.set_filter(1);
    assert(model.visible_devices().size() == 1 && model.selected_id() == "/fixture/imu");
    model.select("imu/gyro");
    assert(model.selected_id() == "/fixture/imu");

    data.sequence = 2;
    data.sensors[2].available = false;
    data.sensors[2].error = "Gyroscope read failed";
    model.ingest(data);
    selected = model.selected_device();
    assert(selected.readings[0]->available && !selected.readings[1]->available);
    assert(selected.readings[1]->error == "Gyroscope read failed");
    assert(std::isfinite(model.history("imu/accel")->points.back().values[0]));
    assert(std::isnan(model.history("imu/gyro")->points.back().values[0]));

    const auto directory = std::filesystem::temp_directory_path() /
                           ("sensors-group-test-" + std::to_string(getpid()));
    assert(model.start_recording(directory));
    ++data.sequence;
    model.ingest(data);
    model.stop_recording();
    assert(model.recorded_rows() == 10);
    std::ifstream recording(model.recording_path());
    std::string line;
    std::getline(recording, line);
    assert(line == "timestamp_utc,elapsed_s,sensor_id,device,bus,channel,value,unit,status,error,simulated,device_id");
    unsigned imu_rows = 0;
    bool recorded_accel = false, recorded_gyro = false;
    const std::string suffix = ",\"/fixture/imu\"";
    while (std::getline(recording, line)) {
        if (line.size() < suffix.size() || line.compare(line.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        ++imu_rows;
        recorded_accel = recorded_accel || line.find("\"imu/accel\"") != std::string::npos;
        recorded_gyro = recorded_gyro || line.find("\"imu/gyro\"") != std::string::npos;
    }
    assert(imu_rows == 6 && recorded_accel && recorded_gyro);
    std::filesystem::remove_all(directory);

    data.sensors.erase(data.sensors.begin());
    ++data.sequence;
    model.ingest(data);
    assert(model.selected_id() == "/fixture/imu");
    assert(model.selected_device().readings.size() == 1 && model.selected()->id == "imu/gyro");
    assert(model.history("imu/accel") == nullptr);
    data.sensors.erase(data.sensors.begin() + 1);
    ++data.sequence;
    model.ingest(data);
    assert(model.selected_device().id.empty() && model.selected_device().readings.empty());
    assert(model.history("imu/gyro") == nullptr);
    model.set_filter(-1);
    assert(model.visible_devices().size() == 2 && model.selected_id() == "/fixture/cpu");

    auto explicit_identity = fixture(1);
    explicit_identity.sensors.front().device_id = "physical-imu";
    gyro.device_id = "physical-imu";
    gyro.path = "/fixture/another-interface";
    explicit_identity.sensors.push_back(gyro);
    SensorsModel explicit_model;
    explicit_model.ingest(explicit_identity);
    assert(explicit_model.selected_id() == "physical-imu");
    assert(explicit_model.selected_device().readings.size() == 2);
    explicit_identity.sensors.front().path.clear();
    explicit_identity.sensors.front().device_id.clear();
    explicit_identity.sensors = {explicit_identity.sensors.front()};
    ++explicit_identity.sequence;
    explicit_model.ingest(explicit_identity);
    assert(explicit_model.selected_id() == "imu/accel");
}
}  // namespace

int main()
{
    physical_device_grouping();
    selected_channel_recording();
    recording_selection_identity();
    SensorsModel model;
    assert(model.selected() == nullptr);
    assert(model.visible_sensors().empty());
    model.ingest(fixture(1));
    assert(model.selected()->id == "imu/accel");
    assert(model.history()->points.size() == 1);
    model.ingest(fixture(1, 99));
    assert(model.history()->points.size() == 1);
    model.ingest(fixture(2, 3));
    assert(model.history()->minimum[0] == 1);
    assert(model.history()->maximum[0] == 3);

    model.set_filter(-2);
    assert(model.visible_sensors().size() == 1);
    assert(model.selected()->id == "cpu/temp");
    model.select("imu/accel");
    assert(model.selected()->id == "cpu/temp");
    model.set_filter(2);
    assert(model.selected() == nullptr);
    model.set_filter(1);
    assert(model.selected()->id == "imu/accel");

    model.set_paused(true);
    model.ingest(fixture(3, 200));
    assert(model.selected()->channels[0].value == 3);
    assert(model.history()->points.size() == 2);
    model.set_paused(false);
    auto broken = fixture(4);
    broken.sensors[0].available = false;
    broken.sensors[0].error = "Input/output error";
    model.ingest(broken);
    assert(!model.selected()->available);
    assert(std::isnan(model.history()->points.back().values[0]));
    assert(model.history()->maximum[0] == 3);
    model.ingest(fixture(5, std::numeric_limits<double>::infinity()));
    assert(!model.selected()->available);
    model.ingest(fixture(6, 4));
    assert(model.selected()->available);
    assert(model.history()->maximum[0] == 4);
    for (unsigned long long seq = 7; seq < 500; ++seq) model.ingest(fixture(seq));
    assert(model.history()->points.size() == 300);
    model.clear_history();
    assert(model.history() == nullptr);
    model.ingest(fixture(500, -4));
    assert(model.history()->points.size() == 1);
    assert(model.history()->maximum[0] == -4);

    const auto directory = std::filesystem::temp_directory_path() /
                           ("sensors-core-test-" + std::to_string(getpid()));
    assert(model.start_recording(directory));
    const auto first_path = model.recording_path();
    auto recorded = fixture(501, 2.125);
    recorded.sensors[0].device = "chip,\"revision\"";
    recorded.simulated = true;
    model.ingest(recorded);
    model.clear_history();
    broken.sequence = 502;
    model.ingest(broken);
    assert(model.recorded_rows() == 8);
    model.stop_recording();
    assert(!model.recording());
    std::ifstream file(first_path);
    std::stringstream content;
    content << file.rdbuf();
    assert(content.str().find("timestamp_utc,elapsed_s,sensor_id") == 0);
    assert(content.str().find("\"chip,\"\"revision\"\"\"") != std::string::npos);
    assert(content.str().find(",2.125,\"m/s2\",ok,\"\",true") != std::string::npos);
    assert(content.str().find(",,\"m/s2\",error,") != std::string::npos);
    assert(content.str().find("nan") == std::string::npos);
    assert(model.start_recording(directory));
    assert(model.recording_path() != first_path);
    model.stop_recording();
    assert(!model.start_recording(std::filesystem::path(first_path) / "invalid"));
    assert(!model.recording_error().empty());

    auto detached = fixture(503);
    detached.sensors.erase(detached.sensors.begin());
    model.ingest(detached);
    assert(model.selected() == nullptr);
    model.set_filter(-1);
    assert(model.selected()->id == "cpu/temp");
    model.set_filter(2);
    assert(model.visible_sensors().empty());
    auto unplugged_bus = fixture(504);
    unplugged_bus.buses.pop_back();
    model.ingest(unplugged_bus);
    assert(model.filter() == -1);
    assert(model.visible_sensors().size() == 2);
    assert(model.selected() != nullptr);
    std::filesystem::remove_all(directory);
    std::cout << "sensor model, filtering, recording and recovery tests passed\n";
}
