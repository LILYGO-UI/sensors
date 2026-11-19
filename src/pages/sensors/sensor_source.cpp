#include "pages/sensors/sensor_source.hpp"
#include "pages/sensors/aht20.hpp"
#include "pages/sensors/i2c_probe.hpp"
#include "pages/sensors/microphone_source.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <iterator>
#include <locale>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace sensors {
namespace {
namespace fs = std::filesystem;

void append_error(std::string &errors, const std::string &error)
{
    if (error.empty()) return;
    if (!errors.empty()) errors += "; ";
    errors += error;
}

bool read_text(const fs::path &path, std::string &result, std::string &error)
{
    result.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        error = path.filename().string() + ": " + std::strerror(errno);
        return false;
    }
    std::array<char, 512> buffer{};
    bool success = true;
    while (true) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            error = path.filename().string() + ": " + std::strerror(errno);
            success = false;
            break;
        }
        if (count == 0) break;
        result.append(buffer.data(), static_cast<std::size_t>(count));
        if (result.size() > 4096) {
            error = path.filename().string() + ": attribute is too long";
            success = false;
            break;
        }
    }
    ::close(fd);
    if (!success) return false;
    const auto first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        error = path.filename().string() + ": empty attribute";
        return false;
    }
    result = result.substr(first, result.find_last_not_of(" \t\r\n") - first + 1);
    return true;
}

std::string optional_text(const fs::path &path)
{
    std::string value, error;
    return read_text(path, value, error) ? value : std::string{};
}

bool read_number(const fs::path &path, double &value, std::string &error)
{
    std::string text;
    if (!read_text(path, text, error)) return false;
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    input >> value;
    if (!input || !std::isfinite(value)) {
        error = path.filename().string() + ": invalid numeric value";
        return false;
    }
    input >> std::ws;
    if (!input.eof()) {
        error = path.filename().string() + ": invalid numeric value";
        return false;
    }
    return true;
}

std::vector<fs::path> entries(const fs::path &directory, std::string &error)
{
    std::vector<fs::path> result;
    std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    if (ec == std::errc::no_such_file_or_directory) return result;
    while (!ec && it != end) {
        result.push_back(it->path());
        it.increment(ec);
    }
    if (ec) append_error(error, directory.string() + ": " + ec.message());
    std::sort(result.begin(), result.end());
    return result;
}

fs::path canonical_path(const fs::path &path)
{
    std::error_code ec;
    auto result = fs::weakly_canonical(path, ec);
    return ec ? path : result;
}

std::string location(const fs::path &device)
{
    static const std::regex client("([0-9]+)-[0-9a-fA-F]{4}");
    static const std::regex adapter("i2c-([0-9]+)");
    for (const auto &path : {canonical_path(device), canonical_path(device / "device")}) {
        for (auto it = path.end(); it != path.begin();) {
            --it;
            std::smatch match;
            const auto part = it->string();
            if (std::regex_match(part, match, client) || std::regex_match(part, match, adapter))
                return "i2c-" + match[1].str();
        }
    }
    return "system";
}

struct Channel {
    fs::path value_path;
    fs::path fault_path;
    std::vector<fs::path> scale_paths;
    std::vector<fs::path> offset_paths;
    double conversion = 1;
    bool raw = false;
};

struct Sensor {
    SensorReading reading;
    std::vector<Channel> channels;
    SensorReading cached;
    Clock::time_point next_sample{};
    bool fast = false;
};

struct Discovery {
    std::vector<Sensor> sensors;
    std::vector<BusInfo> buses;
    std::string error;
#if defined(__linux__) && LILYGO_UI_PLATFORM_K230
    fs::path aht20_adapter;
    std::array<SensorReading, 2> aht20_channels;
    Clock::time_point aht20_next{};
#endif
};

struct Kind {
    const char *attribute;
    const char *category;
    const char *title;
    const char *unit;
    double conversion;
};

constexpr Kind axis_kinds[] = {
    {"accel", "accelerometer", "Accelerometer", "m/s^2", 1},
    {"anglvel", "gyroscope", "Gyroscope", "rad/s", 1},
    {"magn", "magnetometer", "Magnetometer", "uT", 100},
};

// Linux IIO ABI: temperature and humidity are milli-units, pressure is kPa,
// voltage/current/power are mV/mA/mW, and magnetic field is Gauss.
constexpr Kind scalar_kinds[] = {
    {"temp", "temperature", "Temperature", "C", 0.001},
    {"humidityrelative", "humidity", "Humidity", "%", 0.001},
    {"pressure", "pressure", "Pressure", "hPa", 10},
    {"illuminance", "light", "Illuminance", "lx", 1},
    {"voltage", "voltage", "Voltage", "V", 0.001},
    {"current", "current", "Current", "A", 0.001},
    {"power", "power", "Power", "W", 0.001},
};

Sensor make_sensor(const fs::path &directory, const std::string &name,
                   const std::string &key, const Kind &kind)
{
    Sensor result;
    result.reading.id = canonical_path(directory).string() + "/" + key;
    result.reading.name = kind.title;
    result.reading.category = kind.category;
    result.reading.device = name;
    result.reading.location = location(directory);
    result.reading.path = directory.string();
    result.reading.device_id = sensor_device_id(result.reading);
    return result;
}

Channel iio_channel(const fs::path &directory, const std::set<std::string> &files,
                    const std::string &base, const Kind &kind)
{
    Channel result;
    result.raw = files.count(base + "_input") == 0;
    result.value_path = directory / (base + (result.raw ? "_raw" : "_input"));
    result.conversion = kind.conversion;
    for (const auto *suffix : {"_scale", "_offset"}) {
        auto &paths = std::string(suffix) == "_scale" ? result.scale_paths : result.offset_paths;
        paths.push_back(directory / (base + suffix));
        const auto shared = directory / (std::string("in_") + kind.attribute + suffix);
        if (shared != paths.front()) paths.push_back(shared);
    }
    return result;
}

void discover_iio(const SourcePaths &paths, Discovery &result)
{
    static const std::regex device_pattern("iio:device[0-9]+");
    for (const auto &directory : entries(paths.sys / "bus/iio/devices", result.error)) {
        if (!std::regex_match(directory.filename().string(), device_pattern)) continue;
        std::set<std::string> files;
        for (const auto &file : entries(directory, result.error)) files.insert(file.filename().string());
        auto name = optional_text(directory / "name");
        if (name.empty()) name = directory.filename().string();
        for (const auto &kind : axis_kinds) {
            const std::string base = std::string("in_") + kind.attribute;
            bool found = false;
            for (const auto *axis : {"x", "y", "z"}) {
                found = found || files.count(base + "_" + axis + "_raw") ||
                        files.count(base + "_" + axis + "_input");
            }
            if (!found) continue;
            auto sensor = make_sensor(directory, name, base, kind);
            sensor.fast = true;
            for (const auto *axis : {"x", "y", "z"}) {
                sensor.reading.channels.push_back({axis, kind.unit, 0});
                sensor.channels.push_back(iio_channel(directory, files, base + "_" + axis, kind));
            }
            result.sensors.push_back(std::move(sensor));
        }
        for (const auto &kind : scalar_kinds) {
            const std::string prefix = std::string("in_") + kind.attribute;
            const std::regex pattern("(" + prefix + "[0-9]*)(_(ambient|object))?_(raw|input)");
            std::set<std::string> bases;
            for (const auto &file : files) {
                std::smatch match;
                if (std::regex_match(file, match, pattern)) bases.insert(match[1].str() + match[2].str());
            }
            for (const auto &base : bases) {
                auto sensor = make_sensor(directory, name, base, kind);
                auto label = optional_text(directory / (base + "_label"));
                const auto suffix = base.substr(prefix.size());
                if (!label.empty()) sensor.reading.name = label;
                else if (!suffix.empty()) sensor.reading.name += " " + suffix;
                sensor.reading.channels.push_back({kind.title, kind.unit, 0});
                sensor.channels.push_back(iio_channel(directory, files, base, kind));
                result.sensors.push_back(std::move(sensor));
            }
        }
    }
}

void discover_hwmon(const SourcePaths &paths, Discovery &result)
{
    static const std::regex device_pattern("hwmon[0-9]+");
    static const std::regex input_pattern("(temp|in|curr|power|humidity)([0-9]+)_(input|average)");
    constexpr Kind kinds[] = {
        {"temp", "temperature", "Temperature", "C", 0.001},
        {"in", "voltage", "Voltage", "V", 0.001},
        {"curr", "current", "Current", "A", 0.001},
        {"power", "power", "Power", "W", 0.000001},
        {"humidity", "humidity", "Humidity", "%", 0.001},
    };
    for (const auto &directory : entries(paths.sys / "class/hwmon", result.error)) {
        if (!std::regex_match(directory.filename().string(), device_pattern)) continue;
        auto name = optional_text(directory / "name");
        if (name.empty()) name = directory.filename().string();
        std::map<std::string, fs::path> channels;
        for (const auto &file : entries(directory, result.error)) {
            std::smatch match;
            const auto filename = file.filename().string();
            if (!std::regex_match(filename, match, input_pattern)) continue;
            if (match[3] == "average" && match[1] != "power") continue;
            const auto base = match[1].str() + match[2].str();
            if (!channels.count(base) || match[3] == "input") channels[base] = file;
        }
        for (const auto &[base, value_path] : channels) {
            for (const auto &kind : kinds) {
                if (base.find(kind.attribute) != 0) continue;
                auto sensor = make_sensor(directory, name, base, kind);
                auto label = optional_text(directory / (base + "_label"));
                if (label.empty() && name == "aht20") {
                    if (base == "temp1") label = "Ambient temperature";
                    else if (base == "humidity1") label = "Relative humidity";
                }
                sensor.reading.name = label.empty() ? std::string(kind.title) + " " + base.substr(std::strlen(kind.attribute)) : label;
                sensor.reading.channels.push_back({kind.title, kind.unit, 0});
                Channel channel;
                channel.value_path = value_path;
                channel.fault_path = directory / (base + "_fault");
                channel.conversion = kind.conversion;
                sensor.channels.push_back(std::move(channel));
                result.sensors.push_back(std::move(sensor));
                break;
            }
        }
    }
}

fs::path k230_power_client(const SourcePaths &paths, const char *address,
                           std::initializer_list<const char *> names, std::string &error)
{
    const auto bus_root = paths.sys / "bus/i2c/devices";
    static const std::regex pattern("([0-9]+)-([0-9a-f]{4})");
    for (const auto &client : entries(bus_root, error)) {
        std::smatch match;
        const auto id = client.filename().string();
        if (!std::regex_match(id, match, pattern) || match[2] != address) continue;
        const auto name = optional_text(client / "name");
        if (std::find(names.begin(), names.end(), name) != names.end() &&
            optional_text(bus_root / ("i2c-" + match[1].str()) / "name") == "keyboard-i2c") return client;
    }
    return {};
}

template <std::size_t N>
void add_supply_fields(const fs::path &supply, const fs::path &client, const char *model,
                       const Kind (&fields)[N], Discovery &result)
{
    for (const auto &field : fields) {
        const auto attribute = supply / field.attribute;
        std::error_code ec;
        if (!fs::exists(attribute, ec)) continue;
        auto sensor = make_sensor(supply, model, field.attribute, field);
        sensor.reading.path = attribute.string();
        if (!client.empty()) {
            sensor.reading.device_id = canonical_path(client).string();
            sensor.reading.location = location(client);
        }
        if (field.unit) {
            sensor.reading.channels.push_back({field.title, field.unit, 0});
            Channel channel;
            channel.value_path = attribute;
            channel.conversion = field.conversion;
            sensor.channels.push_back(std::move(channel));
        } else {
            sensor.reading.text_value.emplace();
        }
        result.sensors.push_back(std::move(sensor));
    }
}

void add_bq25896(const fs::path &supply, const fs::path &client, bool board_service, Discovery &result)
{
    // The K230 power service publishes VBUS in voltage_now. The kernel driver publishes VBAT.
    const Kind fields[] = {
        {"status", "status", "Charging status", nullptr, 1},
        {"charge_type", "status", "Charge type", nullptr, 1},
        {"health", "status", "Charger health", nullptr, 1},
        {"online", "online", "USB online", "0/1", 1},
        {"voltage_now", "voltage", board_service ? "USB input voltage" : "Battery voltage", "V", 1e-6},
        {"current_now", "current", "Charge current", "mA", 0.001},
        {"input_current_limit", "current_limit", "Input current limit", "mA", 0.001},
        {"constant_charge_current", "current_target", "Charge current setting", "mA", 0.001},
        {"constant_charge_voltage", "voltage_target", "Charge voltage setting", "mV", 0.001},
    };
    add_supply_fields(supply, client, "BQ25896", fields, result);
}

constexpr Kind bq27220_fields[] = {
    {"capacity", "capacity", "State of charge", "%", 1},
    {"status", "status", "Battery status", nullptr, 1},
    {"voltage_now", "voltage", "Battery voltage", "V", 1e-6},
    {"current_now", "current", "Battery current", "mA", 0.001},
    {"temp", "temperature", "Battery temperature", "C", 0.1},
    {"charge_now", "charge", "Remaining capacity", "mAh", 0.001},
    {"charge_full", "charge_full", "Full charge capacity", "mAh", 0.001},
    {"charge_full_design", "charge_design", "Design capacity", "mAh", 0.001},
    {"cycle_count", "cycles", "Cycle count", "cycles", 1},
    {"time_to_empty_now", "time_to_empty", "Time to empty", "min", 1.0 / 60},
    {"time_to_full_now", "time_to_full", "Time to full", "min", 1.0 / 60},
    {"present", "present", "Battery present", "0/1", 1},
    {"technology", "technology", "Battery chemistry", nullptr, 1},
    {"health", "health", "Battery health", nullptr, 1},
    {"power_avg", "power", "Average battery power", "W", 1e-6},
};

bool is_bq27220(const std::string &name)
{
    return name == "bq27220" || name == "BQ27220";
}

void discover_power_supplies(const SourcePaths &paths, Discovery &result)
{
    bool kernel_charger = false;
    std::map<fs::path, std::set<std::string>> native_gauge_channels;
    for (const auto &supply : entries(paths.sys / "class/power_supply", result.error)) {
        const auto model = optional_text(supply / "model_name");
        if (model == "BQ25896") {
            add_bq25896(supply, {}, false, result);
            kernel_charger = true;
        } else if (is_bq27220(model) || (model.empty() && optional_text(supply / "type") == "Battery" &&
                                         is_bq27220(optional_text(supply / "device/name")))) {
            // The bq27xxx kernel driver identifies the gauge through the parent I2C client.
            const auto first = result.sensors.size();
            add_supply_fields(supply, {}, "BQ27220", bq27220_fields, result);
            const auto directory = canonical_path(supply);
            for (std::size_t i = first; i < result.sensors.size(); ++i)
                native_gauge_channels[directory].insert(result.sensors[i].reading.category);
        }
    }
    if (!native_gauge_channels.empty()) {
        // Keep hwmon-only channels while dropping duplicates exported by power_supply.
        const auto hwmon = paths.sys / "class/hwmon";
        result.sensors.erase(std::remove_if(result.sensors.begin(), result.sensors.end(), [&](const Sensor &sensor) {
            const fs::path path = sensor.reading.path;
            if (path.parent_path() != hwmon) return false;
            const auto native = native_gauge_channels.find(canonical_path(path / "device"));
            return native != native_gauge_channels.end() && native->second.count(sensor.reading.category) != 0;
        }), result.sensors.end());
    }
    if (kernel_charger && !native_gauge_channels.empty()) return;

    auto root = paths.k230_power_supply;
#if LILYGO_UI_PLATFORM_K230
    if (root.empty() && paths.sys == LILYGO_UI_DEFAULT_SYSFS_ROOT && paths.dev == LILYGO_UI_DEFAULT_DEV_ROOT) {
        const char *configured = std::getenv("CM0_POWER_SUPPLY_DIR");
        root = configured && *configured ? configured : "/run/lilygo-k230/power_supply";
    }
#endif
    if (root.empty()) return;
    const auto charger = root / "bq25890-charger";
    if (!kernel_charger && optional_text(charger / "model_name") == "BQ25896")
        add_bq25896(charger, k230_power_client(paths, "006b", {"bq25890", "bq25896"}, result.error), true, result);
    const auto gauge = root / "bq27220-battery";
    if (native_gauge_channels.empty() && is_bq27220(optional_text(gauge / "model_name")))
        add_supply_fields(gauge, k230_power_client(paths, "0055", {"bq27220"}, result.error),
                          "BQ27220", bq27220_fields, result);
}

void discover_buses(const SourcePaths &paths, Discovery &result)
{
    static const std::regex adapter_pattern("i2c-([0-9]+)");
    static const std::regex client_pattern("([0-9]+)-([0-9a-fA-F]{4})");
    std::map<int, BusInfo> buses;
    auto number = [](const std::string &value, int base) {
        try { return std::stoi(value, nullptr, base); }
        catch (...) { return -1; }
    };
    for (const auto &directory : entries(paths.sys / "bus/i2c/devices", result.error)) {
        std::smatch match;
        const auto filename = directory.filename().string();
        if (std::regex_match(filename, match, adapter_pattern)) {
            const int bus_number = number(match[1], 10);
            if (bus_number < 0) continue;
            auto &bus = buses[bus_number];
            bus.number = bus_number;
            bus.name = optional_text(directory / "name");
        } else if (std::regex_match(filename, match, client_pattern)) {
            const int bus_number = number(match[1], 10), address = number(match[2], 16);
            if (bus_number < 0 || address < 0) continue;
            auto &bus = buses[bus_number];
            bus.number = bus_number;
            BusDevice device;
            device.address = address;
            device.name = optional_text(directory / "name");
            if ((address == 0x6b && device.name == "bq25890") || (address == 0x55 && device.name == "bq27220")) {
                const char *model = address == 0x55 ? "BQ27220" : "BQ25896";
                const auto id = canonical_path(directory).string();
                if (std::any_of(result.sensors.begin(), result.sensors.end(), [&](const Sensor &sensor) {
                    return sensor.reading.device == model && sensor.reading.device_id == id;
                })) device.name = model;
            }
            if (device.name.empty()) device.name = optional_text(directory / "modalias");
            if (device.name.empty()) device.name = "I2C device";
            std::error_code ec;
            const auto driver = fs::read_symlink(directory / "driver", ec);
            if (!ec) device.driver = driver.filename().string();
            bus.devices.push_back(std::move(device));
        }
    }
    std::string dev_error;
    for (const auto &device : entries(paths.dev, dev_error)) {
        std::smatch match;
        const auto filename = device.filename().string();
        if (!std::regex_match(filename, match, adapter_pattern)) continue;
        const int bus_number = number(match[1], 10);
        if (bus_number >= 0) buses[bus_number].number = bus_number;
    }
    append_error(result.error, dev_error);
    for (auto &[bus_number, bus] : buses) {
        if (bus.name.empty()) bus.name = "i2c-" + std::to_string(bus_number);
        std::sort(bus.devices.begin(), bus.devices.end(), [](const BusDevice &a, const BusDevice &b) {
            return a.address < b.address;
        });
        const auto unbound = std::count_if(bus.devices.begin(), bus.devices.end(), [](const BusDevice &device) {
            return device.driver.empty();
        });
        if (unbound) bus.error = std::to_string(unbound) + " declared device(s) have no bound driver";
        if (paths.probe_i2c) probe_i2c_bus(bus, paths.dev);
        result.buses.push_back(std::move(bus));
    }
}

Discovery discover(const SourcePaths &paths, bool refresh_buses = true)
{
    Discovery result;
    discover_iio(paths, result);
    discover_hwmon(paths, result);
    discover_power_supplies(paths, result);
#if defined(__linux__) && LILYGO_UI_PLATFORM_K230
    // Some K230 kernels declare the client but fail to bind the hwmon driver.
    if (paths.probe_i2c && paths.sys == "/sys" && paths.dev == "/dev") {
        static const std::regex client_pattern("([0-9]+)-0038");
        for (const auto &client : entries(paths.sys / "bus/i2c/devices", result.error)) {
            std::smatch match;
            const auto id = client.filename().string();
            if (!std::regex_match(id, match, client_pattern) || optional_text(client / "name") != "aht20") continue;
            std::error_code ec;
            const auto driver = fs::symlink_status(client / "driver", ec);
            if (driver.type() != fs::file_type::not_found || (ec && ec != std::errc::no_such_file_or_directory)) continue;
            result.aht20_adapter = paths.dev / ("i2c-" + match[1].str());
            for (int i = 0; i < 2; ++i) {
                const auto &kind = scalar_kinds[i];
                auto reading = make_sensor(client, "aht20", kind.attribute, kind).reading;
                reading.name = i == 0 ? "Ambient temperature" : "Relative humidity";
                reading.channels.push_back({kind.title, kind.unit, 0});
                result.aht20_channels[i] = std::move(reading);
            }
            break;
        }
    }
#endif
    if (refresh_buses) discover_buses(paths, result);
    return result;
}

bool read_parameter(const std::vector<fs::path> &paths, bool required, double &value,
                    std::string &error, bool *present = nullptr)
{
    if (present) *present = false;
    for (const auto &path : paths) {
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) continue;
        if (ec) {
            error = path.filename().string() + ": " + ec.message();
            return false;
        }
        // A present but malformed channel scale must not fall back to a shared scale.
        if (present) *present = true;
        return read_number(path, value, error);
    }
    if (required) error = "Missing IIO scale for " + paths.front().filename().string();
    return !required;
}

SensorReading sample_sensor(const Sensor &sensor)
{
    auto reading = sensor.reading;
    if (reading.text_value) {
        reading.available = read_text(reading.path, *reading.text_value, reading.error);
        reading.timestamp = Clock::now();
        return reading;
    }
    reading.available = true;
    for (std::size_t i = 0; i < sensor.channels.size(); ++i) {
        const auto &channel = sensor.channels[i];
        double value = 0, scale = 1, offset = 0;
        if (!channel.fault_path.empty()) {
            double fault = 0;
            if (!read_parameter({channel.fault_path}, false, fault, reading.error)) {
                reading.available = false;
                break;
            }
            if (fault != 0) {
                reading.error = channel.fault_path.filename().string() + ": sensor reports an input fault";
                reading.available = false;
                break;
            }
        }
        if (!read_number(channel.value_path, value, reading.error)) {
            reading.available = false;
            break;
        }
        if (channel.raw) {
            bool scale_present = false, offset_present = false;
            const bool current = reading.category == "current";
            if (!read_parameter(channel.scale_paths, !current, scale, reading.error, &scale_present) ||
                !read_parameter(channel.offset_paths, false, offset, reading.error, &offset_present)) {
                reading.available = false;
                break;
            }
            // IIO current raw values are already mA only when both parameters are absent.
            if (!scale_present && offset_present) {
                reading.error = "Missing IIO scale for " + channel.scale_paths.front().filename().string();
                reading.available = false;
                break;
            }
        }
        if (scale == 0 || (scale < 0 && reading.category != "temperature")) {
            reading.error = channel.value_path.filename().string() +
                            (reading.category == "temperature" ? ": IIO scale must be nonzero" : ": IIO scale must be positive");
            reading.available = false;
            break;
        }
        value = (value + offset) * scale * channel.conversion;
        if (!std::isfinite(value)) {
            reading.error = channel.value_path.filename().string() + ": scaled value is not finite";
            reading.available = false;
            break;
        }
        reading.channels[i].value = value;
    }
    if (!reading.available) {
        for (auto &channel : reading.channels) channel.value = 0;
    }
    reading.timestamp = Clock::now();
    return reading;
}

Snapshot sample(Discovery &discovery, MicrophoneSource *microphone = nullptr)
{
    Snapshot result;
    result.buses = discovery.buses;
    result.error = discovery.error;
    for (auto &sensor : discovery.sensors) {
        const auto now = Clock::now();
        if (now >= sensor.next_sample) {
            sensor.cached = sample_sensor(sensor);
            sensor.next_sample = (sensor.fast && sensor.cached.available)
                                     ? Clock::time_point{}
                                     : Clock::now() + std::chrono::seconds(1);
        }
        result.sensors.push_back(sensor.cached);
    }
    if (microphone) {
        auto readings = microphone->snapshot();
        result.sensors.insert(result.sensors.end(), std::make_move_iterator(readings.begin()),
                              std::make_move_iterator(readings.end()));
    }
#if defined(__linux__) && LILYGO_UI_PLATFORM_K230
    if (!discovery.aht20_adapter.empty()) {
        auto &channels = discovery.aht20_channels;
        if (Clock::now() >= discovery.aht20_next) {
            const auto sample = read_aht20(discovery.aht20_adapter);
            const auto now = Clock::now();
            channels[0].channels[0].value = sample.temperature;
            channels[1].channels[0].value = sample.humidity;
            for (auto &reading : channels) {
                reading.timestamp = now;
                reading.error = sample.error;
                reading.available = sample.error.empty();
            }
            discovery.aht20_next = now + std::chrono::seconds(2);
        }
        result.sensors.insert(result.sensors.end(), channels.begin(), channels.end());
    }
#endif
    if (result.sensors.empty()) append_error(result.error, "No supported sensor channels found");
    result.timestamp = Clock::now();
    return result;
}

Snapshot demo_snapshot(Clock::time_point start)
{
    Snapshot result;
    result.simulated = true;
    result.timestamp = Clock::now();
    const double t = std::chrono::duration<double>(result.timestamp - start).count();
    auto add = [&](const Kind &kind, const std::vector<double> &values, const std::string &device,
                   const std::string &device_id) {
        SensorReading reading;
        reading.id = std::string("demo/") + kind.attribute;
        reading.name = kind.title;
        reading.category = kind.category;
        reading.device = device;
        reading.location = "i2c-1";
        reading.device_id = "demo/i2c-1/" + device_id;
        reading.path = "demo://i2c-1/" + device_id;
        reading.available = true;
        reading.timestamp = result.timestamp;
        for (std::size_t i = 0; i < values.size(); ++i)
            reading.channels.push_back({values.size() == 3 ? std::string(1, "xyz"[i]) : kind.title, kind.unit, values[i]});
        result.sensors.push_back(std::move(reading));
    };
    add(axis_kinds[0], {0.6 * std::sin(t), 0.4 * std::cos(t * 0.7), 9.81 + 0.2 * std::sin(t * 0.4)}, "Demo QMI8658", "1-006a");
    add(axis_kinds[1], {0.25 * std::sin(t * 0.5), 0.3 * std::cos(t * 0.8), 0.15 * std::sin(t)}, "Demo QMI8658", "1-006a");
    add(scalar_kinds[0], {27.5 + 0.6 * std::sin(t * 0.12)}, "Demo environment", "1-0076");
    add(scalar_kinds[1], {46 + 3 * std::sin(t * 0.1)}, "Demo environment", "1-0076");
    add(scalar_kinds[2], {1013.2 + 0.5 * std::sin(t * 0.09)}, "Demo environment", "1-0076");
    add(scalar_kinds[3], {320 + 75 * std::sin(t * 0.3)}, "Demo environment", "1-0076");
    const double rms = -36 + 8 * std::sin(t * 0.8);
    SensorReading waveform;
    waveform.id = "demo/microphone/waveform";
    waveform.device_id = "demo/system/microphone";
    waveform.name = "Raw waveform";
    waveform.category = "microphone";
    waveform.device = std::string("Demo ") + board_microphone_label();
    waveform.location = "system";
    waveform.path = native_microphone_board == MicrophoneBoard::k230 ? "demo://microphone / 48000 Hz / mono" :
                                                                       "demo://microphone / 48000 Hz / left channel";
    waveform.available = true;
    waveform.timestamp = result.timestamp;
    waveform.waveform.reserve(240);
    const double pi = std::acos(-1.0);
    for (std::size_t point = 0; point < 240; ++point)
        waveform.waveform.push_back(0.25 * std::sin(t * 2 + point * 2 * pi / 48));
    result.sensors.push_back(std::move(waveform));

    SensorReading reading;
    reading.id = "demo/microphone/rms";
    reading.device_id = "demo/system/microphone";
    reading.name = "RMS level";
    reading.category = "microphone";
    reading.device = result.sensors.back().device;
    reading.location = "system";
    reading.path = result.sensors.back().path;
    reading.available = true;
    reading.timestamp = result.timestamp;
    reading.channels.push_back({"RMS", "dBFS", rms});
    result.sensors.push_back(std::move(reading));
    /*
     * The microphone device intentionally exposes only the live waveform and RMS;
     * peak and clipping are derived diagnostics, not user-facing measurements.
     */
    result.buses.push_back({0, "Demo I2C 0", "", {}});
    result.buses.push_back({1, "Demo I2C 1", "", {{0x6a, "Demo QMI8658", "demo"}, {0x76, "Demo environment", "demo"}}});
    return result;
}

class PollingSource final : public SensorSource {
public:
    PollingSource(SourcePaths paths, bool demo) : paths_(std::move(paths)), demo_(demo)
    {
        latest_.simulated = demo;
        latest_.error = "Discovering sensors";
        worker_ = std::thread(&PollingSource::run, this);
    }

    ~PollingSource() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    Snapshot snapshot() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

    void rescan() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rescan_ = true;
        }
        wake_.notify_all();
    }

    void set_interval(unsigned milliseconds) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interval_ = std::max(50u, std::min(milliseconds, 60000u));
            interval_changed_ = true;
        }
        wake_.notify_all();
    }

private:
    void run()
    {
        Discovery discovery;
        std::unique_ptr<MicrophoneSource> microphone;
        const auto start = Clock::now();
        auto next_discovery = start;
        unsigned long long sequence = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            const bool refresh_buses = rescan_;
            const bool should_discover = rescan_ || Clock::now() >= next_discovery;
            rescan_ = false;
            interval_changed_ = false;
            lock.unlock();
            Snapshot next;
            try {
                if (demo_) next = demo_snapshot(start);
                else {
                    if (!microphone) microphone = std::make_unique<MicrophoneSource>(paths_);
                    else if (refresh_buses) microphone->rescan();
                    if (should_discover) {
                        auto found = discover(paths_, refresh_buses);
                        if (!refresh_buses) found.buses = discovery.buses;
                        for (auto &sensor : found.sensors) {
                            const auto old = std::find_if(discovery.sensors.begin(), discovery.sensors.end(), [&](const Sensor &previous) {
                                return previous.reading.id == sensor.reading.id;
                            });
                            if (old != discovery.sensors.end()) {
                                sensor.cached = old->cached;
                                sensor.next_sample = old->next_sample;
                            }
                        }
#if defined(__linux__) && LILYGO_UI_PLATFORM_K230
                        if (!found.aht20_adapter.empty() && found.aht20_adapter == discovery.aht20_adapter) {
                            found.aht20_channels = discovery.aht20_channels;
                            found.aht20_next = discovery.aht20_next;
                        }
#endif
                        discovery = std::move(found);
                        next_discovery = Clock::now() + std::chrono::seconds(2);
                    }
                    next = sample(discovery, microphone.get());
                }
            } catch (const std::exception &error) {
                next.error = std::string("Sensor acquisition failed: ") + error.what();
                next.timestamp = Clock::now();
                next.simulated = demo_;
            }
            next.sequence = ++sequence;
            lock.lock();
            latest_ = std::move(next);
            wake_.wait_for(lock, std::chrono::milliseconds(interval_), [this] {
                return stopping_ || rescan_ || interval_changed_;
            });
        }
    }

    SourcePaths paths_;
    bool demo_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;
    Snapshot latest_;
    unsigned interval_ = 100;
    bool stopping_ = false;
    bool rescan_ = true;
    bool interval_changed_ = false;
    std::thread worker_;
};
}  // namespace

std::string sensor_device_id(const SensorReading &reading)
{
    if (!reading.device_id.empty()) return reading.device_id;
    if (reading.path.empty()) return reading.id;
    static const std::regex client("[0-9]+-[0-9a-fA-F]{4}");
    const auto directory = canonical_path(reading.path);
    // IIO and hwmon can expose separate directories belonging to the same I2C client.
    for (auto candidate : {directory, canonical_path(directory / "device")}) {
        while (!candidate.empty()) {
            if (std::regex_match(candidate.filename().string(), client)) return candidate.string();
            const auto parent = candidate.parent_path();
            if (parent == candidate) break;
            candidate = parent;
        }
    }
    return directory.string();
}

Snapshot read_linux_snapshot(const SourcePaths &paths)
{
    static std::atomic<unsigned long long> sequence{0};
    auto discovery = discover(paths);
    auto result = sample(discovery);
    result.sequence = ++sequence;
    return result;
}

std::unique_ptr<SensorSource> make_sensor_source()
{
    SourcePaths paths;
    if (const char *sys = std::getenv("LILYGO_SENSORS_SYS_ROOT"); sys && *sys) paths.sys = sys;
    if (const char *dev = std::getenv("LILYGO_SENSORS_DEV_ROOT"); dev && *dev) paths.dev = dev;
    paths.probe_i2c = paths.sys == LILYGO_UI_DEFAULT_SYSFS_ROOT &&
                      paths.dev == LILYGO_UI_DEFAULT_DEV_ROOT;
    const char *demo = std::getenv("LILYGO_SENSORS_DEMO");
    return std::make_unique<PollingSource>(std::move(paths), demo && std::string(demo) == "1");
}

}  // namespace sensors
