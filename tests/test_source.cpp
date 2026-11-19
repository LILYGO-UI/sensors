#include "pages/sensors/sensor_source.hpp"
#include "pages/sensors/aht20.hpp"
#include "pages/sensors/i2c_probe.hpp"
#include "pages/sensors/sensors_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace sensors;

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    fs::path root;
    SourcePaths paths;

    Fixture()
    {
        std::string pattern = (fs::temp_directory_path() / "lilygo-sensors-XXXXXX").string();
        auto *created = ::mkdtemp(pattern.data());
        require(created != nullptr, "create fixture directory");
        root = created;
        paths = {root / "sys", root / "dev", false};
        fs::create_directories(paths.sys / "bus/iio/devices");
        fs::create_directories(paths.sys / "bus/i2c/devices");
        fs::create_directories(paths.sys / "class/hwmon");
        fs::create_directories(paths.dev);
    }

    ~Fixture() { std::error_code error; fs::remove_all(root, error); }

    void write(const fs::path &path, const std::string &value)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path);
        file << value << '\n';
        require(static_cast<bool>(file), "write fixture " + path.string());
    }

    fs::path iio(const std::string &name = "qmi8658c", int index = 0,
                 const std::string &client = "1-006a")
    {
        const auto path = paths.sys / "devices/platform/soc/3f804000.i2c/i2c-1" / client /
                          ("iio:device" + std::to_string(index));
        write(path / "name", name);
        fs::create_directory_symlink(path, paths.sys / "bus/iio/devices" / path.filename());
        return path;
    }

    fs::path hwmon(const std::string &name, int index)
    {
        const auto path = paths.sys / "class/hwmon" / ("hwmon" + std::to_string(index));
        write(path / "name", name);
        return path;
    }
};

struct Environment {
    struct Value { std::string key, value; bool present; };
    std::vector<Value> previous;
    void set(const std::string &key, const std::string &value)
    {
        const auto *old = std::getenv(key.c_str());
        previous.push_back({key, old ? old : "", old != nullptr});
        ::setenv(key.c_str(), value.c_str(), 1);
    }
    ~Environment()
    {
        for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
            if (it->present) ::setenv(it->key.c_str(), it->value.c_str(), 1);
            else ::unsetenv(it->key.c_str());
        }
    }
};

const SensorReading &find(const Snapshot &snapshot, const std::string &category,
                          const std::string &device = "")
{
    const auto match = std::find_if(snapshot.sensors.begin(), snapshot.sensors.end(), [&](const SensorReading &sensor) {
        return sensor.category == category && (device.empty() || sensor.device == device);
    });
    require(match != snapshot.sensors.end(), "missing sensor " + category + " " + device);
    return *match;
}

void near(double actual, double expected, const std::string &message)
{
    require(std::abs(actual - expected) < 0.00001, message + ": got " + std::to_string(actual));
}

void wait_for(const std::function<bool()> &condition, const std::string &message,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(3500))
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(false, message);
}

void units_and_grouping()
{
    Fixture fixture;
    const auto device = fixture.iio();
    for (const auto *axis : {"x", "y", "z"}) {
        fixture.write(device / (std::string("in_accel_") + axis + "_raw"), "100");
        fixture.write(device / (std::string("in_anglvel_") + axis + "_raw"), "-20");
        fixture.write(device / (std::string("in_magn_") + axis + "_raw"), "2");
    }
    fixture.write(device / "in_accel_scale", "0.002394201");
    fixture.write(device / "in_accel_x_scale", "0.5");
    fixture.write(device / "in_accel_offset", "2");
    fixture.write(device / "in_accel_x_offset", "-1");
    fixture.write(device / "in_anglvel_scale", "0.01");
    fixture.write(device / "in_magn_scale", "0.15");
    fixture.write(device / "in_temp_input", "47535");
    fixture.write(device / "in_temp_raw", "99999");
    fixture.write(device / "in_temp_scale", "99999");
    fixture.write(device / "in_temp1_raw", "200");
    fixture.write(device / "in_temp1_scale", "100");
    fixture.write(device / "in_temp1_offset", "10");
    fixture.write(device / "in_temp1_label", "Ambient probe");
    fixture.write(device / "in_humidityrelative_input", "45100");
    fixture.write(device / "in_pressure_raw", "10132");
    fixture.write(device / "in_pressure_scale", "0.01");
    fixture.write(device / "in_illuminance_input", "456.5");
    fixture.write(device / "in_voltage0_raw", "2000");
    fixture.write(device / "in_voltage_scale", "2");
    fixture.write(device / "in_current0_raw", "-1000");
    fixture.write(device / "in_current_scale", "0.5");
    fixture.write(device / "in_power0_raw", "500");
    fixture.write(device / "in_power_scale", "2");
    fixture.write(device / "in_unknown_raw", "100");
    fixture.write(device / "in_proximity_raw", "100");

    const auto result = read_linux_snapshot(fixture.paths);
    require(result.error.empty() && !result.simulated, "real fixture status");
    require(result.sensors.size() == 11, "group axes, deduplicate input/raw and ignore unknown units");
    const auto &accel = find(result, "accelerometer");
    require(accel.available && accel.channels.size() == 3, "acceleration has three axes");
    require(accel.location == "i2c-1" && accel.channels[0].unit == "m/s^2", "location and acceleration unit");
    near(accel.channels[0].value, 49.5, "channel offset and scale override shared parameters");
    near(accel.channels[1].value, 102 * 0.002394201, "shared offset then scale");
    near(find(result, "gyroscope").channels[0].value, -0.2, "angular velocity stays rad/s");
    near(find(result, "magnetometer").channels[0].value, 30, "Gauss to uT");
    near(find(result, "humidity").channels[0].value, 45.1, "milli percent to percent");
    near(find(result, "pressure").channels[0].value, 1013.2, "kPa to hPa");
    near(find(result, "light").channels[0].value, 456.5, "illuminance stays lux");
    near(find(result, "voltage").channels[0].value, 4, "mV to V");
    near(find(result, "current").channels[0].value, -0.5, "mA to A");
    near(find(result, "power").channels[0].value, 1, "mW to W");
    for (const auto &sensor : result.sensors) {
        require(sensor.device_id == fs::canonical(device.parent_path()).string(), "all measurements share the I2C client identity");
        require(sensor.available && sensor.timestamp != Clock::time_point{}, "timestamp for every sample");
        require(sensor.timestamp <= result.timestamp, "snapshot timestamp follows sensor sampling");
        if (sensor.category == "temperature")
            near(sensor.channels[0].value, sensor.name == "Ambient probe" ? 21 : 47.535, "IIO temperature conversion");
    }
    const auto again = read_linux_snapshot(fixture.paths);
    require(again.sequence > result.sequence && again.timestamp > result.timestamp, "monotonic synchronous snapshots");
}

void physical_device_identity()
{
    Fixture fixture;
    const auto first = fixture.iio();
    fixture.write(first / "in_temp_input", "25000");
    const auto interface = fixture.iio("qmi8658c", 1);
    fixture.write(interface / "in_pressure_input", "101.2");
    const auto twin = fixture.iio("qmi8658c", 2, "1-006b");
    fixture.write(twin / "in_humidityrelative_input", "45100");
    const auto hwmon = fixture.hwmon("qmi8658c", 0);
    fixture.write(hwmon / "in0_input", "3300");
    fs::create_directory_symlink(first.parent_path(), hwmon / "device");
    const auto system = fixture.hwmon("qmi8658c", 1);
    fixture.write(system / "curr1_input", "200");
    const auto result = read_linux_snapshot(fixture.paths);
    require(result.sensors.size() == 5, "all physical-device interfaces remain available as readings");
    const auto &temperature = find(result, "temperature");
    require(temperature.device_id == fs::canonical(first.parent_path()).string(), "I2C identity uses canonical client parent");
    require(find(result, "pressure").device_id == temperature.device_id,
            "multiple IIO interfaces on one client share device identity");
    require(find(result, "voltage").device_id == temperature.device_id,
            "hwmon device symlink joins the IIO physical device");
    require(find(result, "voltage").location == temperature.location, "linked hwmon keeps the physical bus");
    require(find(result, "humidity").device_id == fs::canonical(twin.parent_path()).string() &&
            find(result, "humidity").device_id != temperature.device_id,
            "same-name sensors at different I2C addresses stay distinct");
    require(find(result, "current").device_id == fs::canonical(system).string() &&
            find(result, "current").device_id != temperature.device_id,
            "non-I2C devices fall back to canonical sensor directory, never name");
}

void k230_aht20_hwmon()
{
    std::array<std::uint8_t, 7> frame{0x18, 0x3b, 0x7b, 0xc7, 0x42, 0x30, 0xb2};
    auto direct = decode_aht20(frame);
    require(direct.error.empty(), "accept real K230 AHT20 frame");
    near(direct.temperature, 0x74230 * 200.0 / 1048576 - 50, "AHT20 frame temperature");
    near(direct.humidity, 0x3b7bc * 100.0 / 1048576, "AHT20 frame humidity");
    frame.back() ^= 1;
    require(!decode_aht20(frame).error.empty(), "reject corrupt AHT20 frame");

    Fixture fixture;
    const auto client = fixture.paths.sys / "devices/platform/keyboard-i2c/i2c-7/7-0038";
    fixture.write(client / "name", "aht20");
    fs::create_directory_symlink(client, fixture.paths.sys / "bus/i2c/devices/7-0038");
    require(read_linux_snapshot(fixture.paths).sensors.empty(), "an unbound AHT20 has no measurements");

    const auto hwmon = client / "hwmon/hwmon4";
    fixture.write(hwmon / "name", "aht20");
    fixture.write(hwmon / "temp1_input", "-5230");
    fixture.write(hwmon / "humidity1_input", "67890");
    fs::create_directory_symlink(hwmon, fixture.paths.sys / "class/hwmon/hwmon4");

    const auto result = read_linux_snapshot(fixture.paths);
    const auto &temp = find(result, "temperature", "aht20");
    const auto &rh = find(result, "humidity", "aht20");
    require(result.sensors.size() == 2 && temp.available && rh.available, "both AHT20 channels are readable");
    require(temp.device_id == fs::canonical(client).string() && rh.device_id == temp.device_id &&
            temp.location == "i2c-7" && rh.location == "i2c-7", "both readings follow the K230 I2C adapter");
    require(temp.name == "Ambient temperature" && rh.name == "Relative humidity", "AHT20 measurement labels");
    require(temp.channels.at(0).unit == "C" && rh.channels.at(0).unit == "%", "AHT20 display units");
    near(temp.channels[0].value, -5.23, "AHT20 temperature in Celsius");
    near(rh.channels[0].value, 67.89, "AHT20 relative humidity in percent");

    SensorsModel model;
    model.ingest(result);
    const auto devices = model.visible_devices();
    require(devices.size() == 1 && devices[0].name == "AHT20" && devices[0].readings.size() == 2,
            "K230 shows one AHT20 device with both measurements");

    Fixture cm0;
    const auto imu = cm0.iio();
    cm0.write(imu / "in_temp_input", "30250");
    model.ingest(read_linux_snapshot(cm0.paths));
    const auto cm0_devices = model.visible_devices();
    require(cm0_devices.size() == 1 && cm0_devices[0].name == "qmi8658c",
            "CM0 shows its IMU temperature without an onboard AHT20");
}

void failures_and_reconnection()
{
    Fixture fixture;
    const auto device = fixture.iio();
    for (const auto *axis : {"x", "y", "z"}) fixture.write(device / (std::string("in_accel_") + axis + "_raw"), "20");
    fixture.write(device / "in_accel_scale", "1");
    const auto gauge = fixture.hwmon("bq27220_0", 2);
    for (const auto *channel : {"temp1_input", "in0_input", "curr1_input", "power1_average"})
        fs::create_directory(gauge / channel);
    const auto cpu = fixture.hwmon("cpu_thermal", 0);
    fixture.write(cpu / "temp1_input", "54321");
    const auto alarm = fixture.hwmon("rpi_volt", 1);
    fixture.write(alarm / "in0_lcrit_alarm", "0");

    auto result = read_linux_snapshot(fixture.paths);
    require(result.sensors.size() == 6, "preserve failing gauge channels but ignore alarm-only hwmon");
    near(find(result, "temperature", "cpu_thermal").channels[0].value, 54.321, "hwmon temperature");
    for (const auto &sensor : result.sensors) {
        if (sensor.device != "bq27220_0") continue;
        require(!sensor.available && !sensor.error.empty() && sensor.channels.size() == 1,
                "read failure retains channel metadata and reports error");
    }
    fixture.write(device / "in_accel_x_scale", "garbage");
    result = read_linux_snapshot(fixture.paths);
    require(!find(result, "accelerometer").available, "bad axis scale must not use valid shared scale");
    fs::remove(device / "in_accel_x_scale");
    for (const auto *invalid : {"nan", "inf", "2 3", "1e999", "", "1junk"}) {
        fixture.write(device / "in_accel_y_raw", invalid);
        result = read_linux_snapshot(fixture.paths);
        require(!find(result, "accelerometer").available, "reject invalid raw number");
    }
    fixture.write(device / "in_accel_y_raw", "20");
    for (const auto *invalid : {"0", "-1", "nan", "1 2"}) {
        fixture.write(device / "in_accel_scale", invalid);
        result = read_linux_snapshot(fixture.paths);
        require(!find(result, "accelerometer").available, "reject invalid scale");
    }
    fs::remove(device / "in_accel_scale");
    require(!find(read_linux_snapshot(fixture.paths), "accelerometer").available, "raw without scale is unavailable");
    fixture.write(device / "in_accel_scale", "1");
    fixture.write(device / "in_accel_offset", "invalid");
    require(!find(read_linux_snapshot(fixture.paths), "accelerometer").available, "reject malformed optional offset");
    fs::remove(device / "in_accel_offset");
    fs::remove(device / "in_accel_z_raw");
    result = read_linux_snapshot(fixture.paths);
    require(!find(result, "accelerometer").available && find(result, "accelerometer").channels.size() == 3,
            "missing axis is a retained failed vector");
    fixture.write(device / "in_accel_z_raw", "30");
    require(find(read_linux_snapshot(fixture.paths), "accelerometer").available, "sensor reconnects after attribute returns");
    fs::remove_all(gauge / "power1_average");
    fixture.write(gauge / "power1_average", "1200000");
    fixture.write(gauge / "power1_input", "2500000");
    result = read_linux_snapshot(fixture.paths);
    near(find(result, "power").channels[0].value, 2.5, "hwmon uW to W and input preference");
}

void bq25896_power_supply()
{
    Fixture fixture;
    fixture.paths.k230_power_supply = fixture.root / "run/lilygo-k230/power_supply";
    const auto supply = fixture.paths.k230_power_supply / "bq25890-charger";
    const auto bus = fixture.paths.sys / "bus/i2c/devices";
    const auto client = bus / "7-006b";
    fixture.write(client / "name", "bq25890");
    fixture.write(bus / "i2c-7/name", "keyboard-i2c");
    fixture.write(supply / "model_name", "BQ25890");
    fixture.write(supply / "voltage_now", "4300000");
    fixture.write(fixture.paths.k230_power_supply / "bq27220-battery/model_name", "bq27541");
    fixture.write(fixture.paths.k230_power_supply / "bq27220-battery/current_now", "-111000");
    require(read_linux_snapshot(fixture.paths).sensors.empty(), "charger family name and address alone cannot identify BQ25896");

    fixture.write(supply / "model_name", "BQ25896");
    fixture.write(supply / "current_now", "650000");
    fixture.write(supply / "constant_charge_current", "704000");
    fixture.write(supply / "constant_charge_voltage", "4288000");
    fixture.write(supply / "input_current_limit", "3250000");
    fixture.write(supply / "online", "1");
    fixture.write(supply / "status", "Charging");
    fixture.write(supply / "charge_type", "Fast");
    fixture.write(supply / "health", "Good");
    auto result = read_linux_snapshot(fixture.paths);
    require(!result.simulated && result.sensors.size() == 9, "read charger states and channels from the board service");
    for (const auto &reading : result.sensors)
        require(reading.device == "BQ25896" && reading.available && reading.location == "i2c-7" &&
                reading.device_id == fs::canonical(client).string(), "charger readings share one physical client");
    require(result.buses.at(0).devices.at(0).name == "BQ25896", "bus inventory shows the verified part number");
    const auto &voltage = find(result, "voltage");
    require(voltage.name == "USB input voltage" && voltage.channels[0].unit == "V", "service voltage_now is VBUS");
    near(voltage.channels[0].value, 4.3, "microvolts to volts");
    near(find(result, "current").channels[0].value, 650, "measured charge current in mA");
    near(find(result, "current_target").channels[0].value, 704, "configured current is separate");
    near(find(result, "current_limit").channels[0].value, 3250, "input current limit in mA");
    near(find(result, "voltage_target").channels[0].value, 4288, "regulation setting in mV");
    require(find(result, "status").text_value == "Charging", "keep charging state as text");

    fs::remove(supply / "voltage_now");
    fixture.write(supply / "current_now", "invalid");
    fixture.write(supply / "online", "0");
    fixture.write(supply / "status", "Discharging");
    result = read_linux_snapshot(fixture.paths);
    require(result.sensors.size() == 8 && !find(result, "current").available && find(result, "online").available &&
            find(result, "online").channels[0].value == 0 && find(result, "status").text_value == "Discharging",
            "missing ADC attributes, bad samples and offline state stay distinct");

    const auto kernel = fixture.paths.sys / "class/power_supply/bq25890-charger-0";
    fixture.write(kernel / "model_name", "BQ25896");
    fixture.write(kernel / "voltage_now", "4100000");
    fs::create_directory_symlink(client, kernel / "device");
    result = read_linux_snapshot(fixture.paths);
    require(result.sensors.size() == 1 && result.sensors[0].name == "Battery voltage" &&
            result.sensors[0].device_id == fs::canonical(client).string(), "native VBAT takes precedence over the mirror");
    near(result.sensors[0].channels[0].value, 4.1, "native battery voltage in V");
}

void bq27220_power_supply()
{
    Fixture fixture;
    const auto root = fixture.root / "run/lilygo-k230/power_supply";
    fixture.paths.k230_power_supply = root;
    const auto gauge = root / "bq27220-battery";
    const auto bus = fixture.paths.sys / "bus/i2c/devices";
    const auto client = bus / "9-0055";
    fixture.write(bus / "i2c-9/name", "keyboard-i2c");
    fixture.write(client / "name", "bq27220");
    fixture.write(bus / "9-006b/name", "bq25890");
    fixture.write(root / "bq25890-charger/model_name", "BQ25896");
    fixture.write(root / "bq25890-charger/current_now", "650000");
    fixture.write(gauge / "capacity", "95");
    auto snapshot = read_linux_snapshot(fixture.paths);
    require(snapshot.sensors.size() == 1 && snapshot.sensors.front().device == "BQ25896",
            "a gauge directory or declared I2C client alone does not identify a live gauge");
    fixture.write(gauge / "model_name", "bq27220");
    fixture.write(gauge / "voltage_now", "4211000");
    fixture.write(gauge / "current_now", "-123000");
    fixture.write(gauge / "temp", "366");
    fixture.write(gauge / "charge_now", "3066000");
    fixture.write(gauge / "charge_full", "3253000");
    fixture.write(gauge / "charge_full_design", "3000000");
    fixture.write(gauge / "cycle_count", "1");
    fixture.write(gauge / "time_to_empty_now", "7200");
    fixture.write(gauge / "status", "Discharging");
    snapshot = read_linux_snapshot(fixture.paths);
    const auto check = [&](const char *category, const char *unit, double expected) {
        const auto &r = find(snapshot, category, "BQ27220");
        require(r.available && r.device_id == fs::canonical(client).string() && r.location == "i2c-9" &&
                r.channels.at(0).unit == unit, std::string("gauge identity and units: ") + category);
        near(r.channels[0].value, expected, category);
    };
    check("capacity", "%", 95);
    check("voltage", "V", 4.211);
    check("current", "mA", -123);
    check("temperature", "C", 36.6);
    check("charge", "mAh", 3066);
    check("charge_full", "mAh", 3253);
    check("charge_design", "mAh", 3000);
    check("cycles", "cycles", 1);
    check("time_to_empty", "min", 120);
    require(find(snapshot, "status", "BQ27220").text_value == "Discharging" &&
            snapshot.buses.at(0).devices.at(0).name == "BQ27220", "gauge status and bus inventory");
    SensorsModel model;
    model.ingest(snapshot);
    require(model.visible_devices().size() == 2, "the gauge and the charger have distinct detail pages");
    fs::remove(gauge / "time_to_empty_now");
    fixture.write(gauge / "time_to_full_now", "900");
    fixture.write(gauge / "current_now", "500000");
    fixture.write(gauge / "temp", "unreadable");
    snapshot = read_linux_snapshot(fixture.paths);
    check("time_to_full", "min", 15);
    check("current", "mA", 500);
    require(!find(snapshot, "temperature", "BQ27220").available && std::none_of(snapshot.sensors.begin(),
        snapshot.sensors.end(), [](const SensorReading &r) { return r.category == "time_to_empty"; }),
        "unavailable estimates disappear and invalid samples keep their error state");

    // The native bq27xxx driver has no model_name and may also publish the same voltage through hwmon.
    const auto native = fixture.paths.sys / "class/power_supply/bq27220-0";
    fixture.write(native / "type", "Battery");
    fs::create_directory_symlink(client, native / "device");
    fixture.write(native / "voltage_now", "4100000");
    const auto mirror = fixture.hwmon("bq27220_0", 0);
    fixture.write(mirror / "in0_input", "4100");
    fs::create_directory_symlink(native, mirror / "device");
    snapshot = read_linux_snapshot(fixture.paths);
    require(snapshot.sensors.size() == 2 && find(snapshot, "current", "BQ25896").available,
            "native gauge replaces its mirrors while the service charger remains visible");
    check("voltage", "V", 4.1);
}

void iio_scale_exceptions()
{
    Fixture fixture;
    const auto device = fixture.iio("adis16209");
    fixture.write(device / "in_temp_raw", "10");
    fixture.write(device / "in_temp_scale", "-470");
    fixture.write(device / "in_temp_offset", "-60");
    fixture.write(device / "in_current0_raw", "-1250");

    auto result = read_linux_snapshot(fixture.paths);
    require(find(result, "temperature").available, "negative temperature scale is valid");
    near(find(result, "temperature").channels[0].value, 23.5, "negative temperature scale applies after offset");
    require(find(result, "current").available, "current without scale or offset is already processed");
    near(find(result, "current").channels[0].value, -1.25, "processed IIO current mA to A");

    for (const auto *invalid : {"0", "nan", "-inf", "-470junk"}) {
        fixture.write(device / "in_temp_scale", invalid);
        require(!find(read_linux_snapshot(fixture.paths), "temperature").available,
                "temperature still rejects zero and malformed scales");
    }
    fixture.write(device / "in_current_offset", "10");
    require(!find(read_linux_snapshot(fixture.paths), "current").available,
            "current with offset alone must not assume a scale");
    fs::remove(device / "in_current_offset");
    fixture.write(device / "in_current_scale", "0.5");
    fixture.write(device / "in_current0_scale", "broken");
    require(!find(read_linux_snapshot(fixture.paths), "current").available,
            "malformed current scale must not use shared scale or processed fallback");
    fs::remove(device / "in_current0_scale");
    fixture.write(device / "in_current_offset", "250");
    result = read_linux_snapshot(fixture.paths);
    require(find(result, "current").available, "current uses available scale and offset");
    near(find(result, "current").channels[0].value, -0.5, "current offset and scale conversion");
    fixture.write(device / "in_current_offset", "invalid");
    require(!find(read_linux_snapshot(fixture.paths), "current").available,
            "malformed current offset is not ignored");
}

void hwmon_input_faults()
{
    Fixture fixture;
    const auto device = fixture.hwmon("temperature_probe", 0);
    fixture.write(device / "temp1_input", "25000");
    fixture.write(device / "temp1_fault", "1");
    auto result = read_linux_snapshot(fixture.paths);
    const auto &failed = find(result, "temperature");
    require(!failed.available && failed.error.find("temp1_fault") != std::string::npos,
            "disconnected probe fault overrides plausible input reading");
    require(failed.channels.size() == 1 && failed.channels[0].unit == "C",
            "fault retains channel metadata");
    fixture.write(device / "temp1_fault", "0");
    fixture.write(device / "temp1_max_alarm", "1");
    result = read_linux_snapshot(fixture.paths);
    require(find(result, "temperature").available, "threshold alarm is not an input fault");
    near(find(result, "temperature").channels[0].value, 25, "probe recovers when fault clears");
    fixture.write(device / "temp1_fault", "invalid");
    require(!find(read_linux_snapshot(fixture.paths), "temperature").available,
            "malformed fault attribute makes reading untrusted");
    fs::remove(device / "temp1_fault");
    fs::create_directory(device / "temp1_fault");
    require(!find(read_linux_snapshot(fixture.paths), "temperature").available,
            "unreadable fault attribute makes reading untrusted");
    fs::remove(device / "temp1_fault");
    require(find(read_linux_snapshot(fixture.paths), "temperature").available,
            "missing optional fault attribute does not invalidate reading");
}

void bus_inventory()
{
    Fixture fixture;
    const auto busroot = fixture.paths.sys / "bus/i2c/devices";
    fixture.write(busroot / "i2c-0/name", "Camera I2C");
    fixture.write(busroot / "i2c-1/name", "Sensor I2C");
    fixture.write(busroot / "i2c-2/name", "Auxiliary I2C");
    fixture.write(busroot / "1-006a/name", "qmi8658b");
    fixture.write(busroot / "1-0055/name", "bq27220");
    fixture.write(busroot / "0-005d/name", "gt9895");
    const auto driver = fixture.paths.sys / "bus/i2c/drivers/qmi8658";
    fs::create_directories(driver);
    fs::create_directory_symlink(driver, busroot / "1-006a/driver");
    fixture.write(fixture.paths.dev / "i2c-3", "");
    const auto result = read_linux_snapshot(fixture.paths);
    require(result.buses.size() == 4, "sysfs and dev adapters merge including empty buses");
    require(result.buses[0].number == 0 && result.buses[1].number == 1, "bus ordering");
    require(result.buses[1].name == "Sensor I2C", "adapter name");
    require(result.buses[1].devices.size() == 2 && result.buses[1].devices[0].address == 0x55, "address ordering");
    require(result.buses[1].devices[0].driver.empty() && !result.buses[1].error.empty(), "declared device reports no driver");
    require(result.buses[1].devices[1].driver == "qmi8658", "bound driver resolved");
    require(result.buses[2].devices.empty() && result.buses[2].error.empty(), "empty adapter is not an I/O error");
    require(!result.simulated && !result.error.empty(), "no sensor state is never implicit demo");
}

void unregistered_i2c_devices()
{
    BusInfo bus{0, "External sensors", "1 declared device(s) have no bound driver",
                {{0x5d, "gt9895", "goodix"}, {0x1a, "imx219", ""}}};
    std::vector<int> visited;
    probe_i2c_addresses(bus, [&](int address) -> AddressProbeResult {
        visited.push_back(address);
        require(address >= 0x03 && address <= 0x77, "reserved addresses are excluded");
        require(address != 0x5d && address != 0x1a, "registered clients are never probed");
        if (address == 0x44 || address == 0x76) return {AddressStatus::acknowledged, {}};
        if (address == 0x68) return {AddressStatus::busy, {}};
        return {AddressStatus::absent, {}};
    });
    require(visited.size() == 115, "all unregistered standard addresses are probed on bus zero");
    require(bus.devices.size() == 5, "unregistered sensors merge with registered devices");
    require(bus.devices[1].address == 0x44 && bus.devices[1].status == BusDeviceStatus::acknowledged,
            "ACK is reported independently of driver registration");
    require(bus.devices[1].name == "Unknown I2C device", "address alone does not identify a sensor model");
    require(bus.devices[2].driver == "goodix", "registered driver information is preserved");
    require(bus.devices[3].address == 0x68 && bus.devices[3].status == BusDeviceStatus::busy,
            "busy addresses are distinguished from hardware responses");
    require(bus.error == "1 declared device(s) have no bound driver", "existing diagnostics survive probing");

    probe_i2c_addresses(bus, [](int address) -> AddressProbeResult {
        return {address == 0x76 ? AddressStatus::acknowledged : AddressStatus::absent, {}};
    });
    require(bus.devices.size() == 3 && bus.devices.back().address == 0x76,
            "rescan removes unplugged devices and avoids duplicates");

    BusInfo failed{0, "I2C 0", "", {{0x5d, "gt9895", "goodix"}}};
    unsigned attempts = 0;
    probe_i2c_addresses(failed, [&](int) -> AddressProbeResult {
        ++attempts;
        return {AddressStatus::failed, "Permission denied"};
    });
    require(attempts == 1 && failed.devices.size() == 1 && failed.error.find("Permission denied") != std::string::npos,
            "adapter failure stops scanning, reports the cause and preserves registered devices");
    BusInfo partial;
    probe_i2c_addresses(partial, [](int address) -> AddressProbeResult {
        if (address == 0x44) return {AddressStatus::acknowledged, {}};
        return {AddressStatus::unsupported, {}};
    });
    require(partial.devices.size() == 1 && !partial.error.empty(),
            "unsupported transactions report incomplete coverage without hiding responses");
}

void worker_controls()
{
    Fixture fixture;
    Environment environment;
    environment.set("LILYGO_SENSORS_SYS_ROOT", fixture.paths.sys.string());
    environment.set("LILYGO_SENSORS_DEV_ROOT", fixture.paths.dev.string());
    environment.set("LILYGO_SENSORS_DEMO", "0");
    auto source = make_sensor_source();
    wait_for([&] { return source->snapshot().sequence > 0; }, "initial worker sample");
    const auto missing = source->snapshot();
    require(missing.sensors.size() == 2 && !missing.simulated, "missing hardware retains microphone diagnostics");
    for (const auto &reading : missing.sensors) {
        require(reading.category == "microphone" && !reading.available && !reading.error.empty(),
                "missing microphone never generates real measurements");
    }
    const auto device = fixture.iio();
    fixture.write(device / "in_temp_input", "23000");
    source->rescan();
    wait_for([&] { return source->snapshot().sensors.size() == 3; }, "rescan detects attached device");
    auto first = source->snapshot();
    const auto first_timestamp = first.sensors[0].timestamp;
    fixture.write(device / "in_temp_input", "24000");
    wait_for([&] { return source->snapshot().sequence >= first.sequence + 2; }, "worker continues publishing");
    auto cached = source->snapshot();
    near(cached.sensors[0].channels[0].value, 23, "slow channel is cached for one second");
    require(cached.timestamp > first.timestamp && cached.sensors[0].timestamp == first_timestamp,
            "cache preserves sample timestamp while snapshot advances");
    wait_for([&] { return source->snapshot().sensors[0].channels[0].value == 24; }, "slow channel updates after cache expires");
    fixture.write(device / "in_temp_input", "not-a-number");
    wait_for([&] { return !source->snapshot().sensors[0].available; }, "worker reports per-sensor failure");
    fixture.write(device / "in_temp_input", "25000");
    wait_for([&] { return source->snapshot().sensors[0].available; }, "worker retries failed channel");
    near(source->snapshot().sensors[0].channels[0].value, 25, "recovered value");
    const auto second = fixture.iio("ambient", 1);
    fixture.write(second / "in_pressure_input", "101.2");
    wait_for([&] { return source->snapshot().sensors.size() == 4; }, "periodic discovery detects new device");
    fs::remove_all(second);
    source->rescan();
    wait_for([&] { return source->snapshot().sensors.size() == 3; }, "rescan removes disconnected device");
    source->set_interval(250);
    auto before = source->snapshot().sequence;
    wait_for([&] { return source->snapshot().sequence > before; }, "interval update wakes worker");
    source->set_interval(1000);
    before = source->snapshot().sequence;
    wait_for([&] { return source->snapshot().sequence > before; }, "slow interval wakes worker");
    const auto stop_start = Clock::now();
    source.reset();
    require(Clock::now() - stop_start < std::chrono::milliseconds(500), "destructor interrupts polling wait");
    environment.set("LILYGO_SENSORS_DEMO", "1");
    source = make_sensor_source();
    wait_for([&] { return source->snapshot().sequence > 0; }, "explicit demo publishes sample");
    require(source->snapshot().simulated && source->snapshot().sensors.size() == 8, "explicit demo only");
    const auto demo = source->snapshot();
    require(find(demo, "accelerometer").device_id == find(demo, "gyroscope").device_id,
            "demo IMU measurements share one device");
    require(find(demo, "temperature").device_id == find(demo, "humidity").device_id &&
            find(demo, "pressure").device_id == find(demo, "light").device_id &&
            find(demo, "temperature").device_id == find(demo, "pressure").device_id,
            "demo environment measurements share one device");
    require(find(demo, "accelerometer").device_id != find(demo, "temperature").device_id,
            "demo physical devices stay distinct");
    std::string microphone_id;
    unsigned microphone_count = 0;
    for (const auto &reading : demo.sensors) {
        if (reading.category != "microphone") continue;
        if (microphone_id.empty()) microphone_id = reading.device_id;
        require(reading.device_id == microphone_id && reading.location == "system" && reading.available,
                "demo microphone levels share a system device");
        if (reading.name == "Raw waveform") {
            require(reading.channels.empty() && reading.waveform.size() == 240,
                    "demo includes a decimated raw waveform");
        } else {
            require(reading.channels.size() == 1 && reading.channels[0].unit == "dBFS",
                    "demo RMS keeps its unit");
        }
        ++microphone_count;
    }
    require(microphone_count == 2, "demo includes waveform and RMS measurements");
}
}  // namespace

int main()
{
    try {
        units_and_grouping();
        physical_device_identity();
        k230_aht20_hwmon();
        bq25896_power_supply();
        bq27220_power_supply();
        failures_and_reconnection();
        iio_scale_exceptions();
        hwmon_input_faults();
        bus_inventory();
        unregistered_i2c_devices();
        worker_controls();
        std::cout << "sensor source tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "sensor source test failure: " << error.what() << '\n';
        return 1;
    }
}
