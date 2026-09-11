#pragma once

#include "pages/sensors/sensor_source.hpp"

#include <array>
#include <deque>
#include <fstream>
#include <map>
#include <set>

namespace sensors {

struct SamplePoint {
    double seconds = 0;
    std::vector<double> values;
};

struct SensorHistory {
    std::deque<SamplePoint> points;
    std::vector<double> minimum;
    std::vector<double> maximum;
};

struct SensorDevice {
    std::string id;
    std::string name;
    std::string location;
    std::string path;
    std::vector<const SensorReading *> readings;
};

class SensorsModel {
public:
    void ingest(Snapshot snapshot);
    void select(const std::string &id);
    void set_filter(int bus);
    void set_paused(bool paused);
    void clear_history();
    bool channel_recording_enabled(const std::string &reading_id, std::size_t channel_index) const;
    void set_channel_recording_enabled(const std::string &reading_id, std::size_t channel_index, bool enabled);
    std::size_t recording_channel_count() const;
    bool start_recording(const std::filesystem::path &directory);
    void stop_recording();
    bool recording() const { return recording_.is_open(); }
    bool paused() const { return paused_; }
    int filter() const { return filter_; }
    const Snapshot &snapshot() const { return snapshot_; }
    const SensorReading *selected() const;
    SensorDevice selected_device() const;
    const SensorHistory *history() const;
    const SensorHistory *history(const std::string &reading_id) const;
    const std::string &selected_id() const { return selected_id_; }
    std::vector<const SensorReading *> visible_sensors() const;
    std::vector<SensorDevice> visible_devices() const;
    const std::string &recording_path() const { return recording_path_; }
    const std::string &recording_error() const { return recording_error_; }
    unsigned long long recorded_rows() const { return recorded_rows_; }

private:
    Snapshot snapshot_;
    std::map<std::string, SensorHistory> histories_;
    std::set<std::array<std::string, 4>> disabled_recording_channels_;
    std::string selected_id_;
    int filter_ = -1;
    bool paused_ = false;
    Clock::time_point origin_{};
    unsigned long long last_sequence_ = 0;
    std::ofstream recording_;
    std::string recording_path_;
    std::string recording_error_;
    unsigned long long recorded_rows_ = 0;
};

}  // namespace sensors
