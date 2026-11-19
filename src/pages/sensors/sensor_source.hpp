#pragma once

#include "platform_paths.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sensors {

using Clock = std::chrono::steady_clock;

struct ChannelReading {
    std::string name;
    std::string unit;
    double value = 0;
};

struct SensorReading {
    std::string id;
    std::string name;
    std::string category;
    std::string device;
    std::string location;
    std::string path;
    std::string error;
    bool available = false;
    std::vector<ChannelReading> channels;
    Clock::time_point timestamp{};
    std::string device_id;
    // Optional current waveform samples, normalized to signed full scale [-1, 1].
    std::vector<double> waveform;
    // Discrete states such as a charger's health have a text value instead of numeric channels.
    std::optional<std::string> text_value{};
};

std::string sensor_device_id(const SensorReading &reading);

enum class BusDeviceStatus { registered, acknowledged, busy };

struct BusDevice {
    int address = 0;
    std::string name;
    std::string driver;
    BusDeviceStatus status = BusDeviceStatus::registered;
};

struct BusInfo {
    int number = 0;
    std::string name;
    std::string error;
    std::vector<BusDevice> devices;
};

struct Snapshot {
    std::vector<SensorReading> sensors;
    std::vector<BusInfo> buses;
    std::string error;
    bool simulated = false;
    unsigned long long sequence = 0;
    Clock::time_point timestamp{};
};

class SensorSource {
public:
    virtual ~SensorSource() = default;
    virtual Snapshot snapshot() = 0;
    virtual void rescan() = 0;
    virtual void set_interval(unsigned milliseconds) = 0;
};

// Root overrides support deterministic sysfs fixtures without touching hardware.
struct SourcePaths {
    std::filesystem::path sys = LILYGO_UI_DEFAULT_SYSFS_ROOT;
    std::filesystem::path dev = LILYGO_UI_DEFAULT_DEV_ROOT;
    bool probe_i2c = true;
    std::filesystem::path k230_power_supply{};  // Optional board-service root for offline fixtures.
};

// One-shot sysfs reads; continuous audio capture is provided by make_sensor_source().
Snapshot read_linux_snapshot(const SourcePaths &paths = {});
std::unique_ptr<SensorSource> make_sensor_source();

}  // namespace sensors
