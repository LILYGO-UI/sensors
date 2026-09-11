#include "app.hpp"

#include "pages/sensors/sensor_source.hpp"
#include "pages/keyboard/keyboard_source.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <locale>
#include <string>
#include <string_view>
#include <thread>

namespace {

void json_string(std::ostream &out, const std::string &value)
{
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            out << '\\' << static_cast<char>(character);
        } else if (character < 0x20) {
            out << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
        } else {
            out << static_cast<char>(character);
        }
    }
    out << '"';
}

void json_field(std::ostream &out, const char *key, const std::string &value)
{
    out << ",\"" << key << "\":";
    json_string(out, value);
}

void print_snapshot(const sensors::Snapshot &snapshot)
{
    auto &out = std::cout;
    out << "{\"schema_version\":1,\"sequence\":" << snapshot.sequence
        << ",\"simulated\":" << (snapshot.simulated ? "true" : "false")
        << ",\"timestamp_monotonic_ms\":"
        << std::chrono::duration_cast<std::chrono::milliseconds>(snapshot.timestamp.time_since_epoch()).count();
    json_field(out, "error", snapshot.error);
    out << ",\"sensors\":[";
    bool first_sensor = true;
    for (const auto &sensor : snapshot.sensors) {
        if (!first_sensor) out << ',';
        first_sensor = false;
        out << "{\"id\":";
        json_string(out, sensor.id);
        json_field(out, "name", sensor.name);
        json_field(out, "category", sensor.category);
        json_field(out, "device", sensor.device);
        json_field(out, "device_id", sensor.device_id);
        json_field(out, "location", sensor.location);
        json_field(out, "path", sensor.path);
        json_field(out, "error", sensor.error);
        out << ",\"available\":" << (sensor.available ? "true" : "false")
            << ",\"timestamp_monotonic_ms\":"
            << std::chrono::duration_cast<std::chrono::milliseconds>(sensor.timestamp.time_since_epoch()).count()
            << ",\"channels\":[";
        bool first_channel = true;
        for (const auto &channel : sensor.channels) {
            if (!first_channel) out << ',';
            first_channel = false;
            out << "{\"name\":";
            json_string(out, channel.name);
            json_field(out, "unit", channel.unit);
            out << ",\"value\":";
            if (sensor.available && std::isfinite(channel.value)) out << std::setprecision(12) << channel.value;
            else out << "null";
            out << '}';
        }
        out << "],\"waveform\":[";
        for (std::size_t index = 0; index < sensor.waveform.size(); ++index) {
            if (index) out << ',';
            out << std::setprecision(9) << sensor.waveform[index];
        }
        out << "]}";
    }
    out << "],\"buses\":[";
    bool first_bus = true;
    for (const auto &bus : snapshot.buses) {
        if (!first_bus) out << ',';
        first_bus = false;
        out << "{\"number\":" << bus.number;
        json_field(out, "name", bus.name);
        json_field(out, "error", bus.error);
        out << ",\"devices\":[";
        bool first_device = true;
        for (const auto &device : bus.devices) {
            if (!first_device) out << ',';
            first_device = false;
            out << "{\"address\":" << device.address;
            json_field(out, "name", device.name);
            json_field(out, "driver", device.driver);
            json_field(out, "status", device.status == sensors::BusDeviceStatus::acknowledged ? "acknowledged" :
                                     device.status == sensors::BusDeviceStatus::busy ? "busy" : "registered");
            out << '}';
        }
        out << "]}";
    }
    out << "]}\n" << std::flush;
}

bool parse_unsigned(std::string_view value, unsigned minimum, unsigned maximum, unsigned &result)
{
    unsigned parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
        parsed < minimum || parsed > maximum) return false;
    result = parsed;
    return true;
}

void usage(std::ostream &out)
{
    out << "Usage: lilygo-ui-sensors [AppKit display options]\n"
           "       lilygo-ui-sensors --probe [--samples N] [--interval MS]\n"
           "       lilygo-ui-sensors --keyboard-probe [--samples N] [--interval MS]\n"
           "Probe prints one JSON object per sample without opening a display.\n"
           "N: 1..10000 (default 1); MS: 50..60000 (default 250).\n";
}

int probe(unsigned samples, unsigned interval)
{
    std::cout.imbue(std::locale::classic());
    auto source = sensors::make_sensor_source();
    source->set_interval(interval);
    sensors::Snapshot snapshot;
    const auto deadline = sensors::Clock::now() + std::chrono::seconds(5);
    do {
        snapshot = source->snapshot();
        if (snapshot.sequence != 0 || snapshot.timestamp != sensors::Clock::time_point{}) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (sensors::Clock::now() < deadline);
    if (snapshot.sequence == 0 && snapshot.timestamp == sensors::Clock::time_point{}) {
        snapshot.error = "Timed out waiting for the first sensor snapshot";
        snapshot.timestamp = sensors::Clock::now();
        print_snapshot(snapshot);
        return 1;
    }

    bool any_valid_reading = false;
    for (unsigned index = 0; index < samples; ++index) {
        if (index != 0) {
            const auto previous_sequence = snapshot.sequence;
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            const auto next_deadline = sensors::Clock::now() + std::chrono::seconds(5);
            do {
                snapshot = source->snapshot();
                if (snapshot.sequence != previous_sequence) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } while (sensors::Clock::now() < next_deadline);
            if (snapshot.sequence == previous_sequence) {
                snapshot.error = "Timed out waiting for a new sensor snapshot";
                for (auto &sensor : snapshot.sensors) {
                    sensor.available = false;
                    sensor.error = snapshot.error;
                }
            }
        }
        print_snapshot(snapshot);
        for (const auto &sensor : snapshot.sensors) {
            any_valid_reading |= sensor.available &&
                std::any_of(sensor.channels.begin(), sensor.channels.end(),
                            [](const auto &channel) { return std::isfinite(channel.value); });
        }
    }
    return any_valid_reading ? 0 : 1;
}

int keyboard_probe(unsigned samples, unsigned interval)
{
    auto source = sensors::make_keyboard_source();
    source->set_active(true);
    bool connected = false;
    for (unsigned index = 0; index < samples; ++index) {
        if (index) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        const auto snapshot = source->snapshot();
        connected |= snapshot.monitoring;
        auto &out = std::cout;
        out << "{\"schema_version\":1,\"type\":\"keyboard\",\"sequence\":" << snapshot.sequence
            << ",\"monitoring\":" << (snapshot.monitoring ? "true" : "false");
        json_field(out, "error", snapshot.error);
        json_field(out, "selected_id", snapshot.selected_id);
        out << ",\"devices\":[";
        bool first = true;
        for (const auto &device : snapshot.devices) {
            if (!first) out << ',';
            first = false;
            out << "{\"id\":";
            json_string(out, device.id);
            json_field(out, "name", device.name);
            json_field(out, "path", device.path);
            json_field(out, "location", device.location);
            json_field(out, "error", device.error);
            out << ",\"available\":" << (device.available ? "true" : "false") << '}';
        }
        out << "],\"pressed\":[";
        first = true;
        for (const auto code : snapshot.pressed) {
            if (!first) out << ',';
            first = false;
            out << code;
        }
        out << "],\"events\":[";
        first = true;
        for (const auto &event : snapshot.events) {
            if (!first) out << ',';
            first = false;
            out << "{\"code\":" << event.code << ",\"value\":" << event.value
                << ",\"timestamp_ms\":" << event.timestamp_ms;
            json_field(out, "name", event.name);
            out << '}';
        }
        out << "]}\n" << std::flush;
    }
    source->set_active(false);
    return connected ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv)
{
    bool probe_requested = false, keyboard_requested = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--probe") probe_requested = true;
        if (std::string_view(argv[index]) == "--keyboard-probe") keyboard_requested = true;
        if (std::string_view(argv[index]) == "--help") {
            usage(std::cout);
            return 0;
        }
    }
    if (probe_requested && keyboard_requested) {
        std::cerr << "Choose --probe or --keyboard-probe\n";
        return 2;
    }
    if (!probe_requested && !keyboard_requested) {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--samples" || argument == "--interval") {
                std::cerr << argument << " requires --probe or --keyboard-probe\n";
                return 2;
            }
        }
        return cm0_app_run(argc, argv, cm0_app_get_descriptor());
    }

    unsigned samples = 1, interval = 250;
    bool have_samples = false, have_interval = false, have_probe = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if ((argument == "--probe" || argument == "--keyboard-probe") && !have_probe) {
            have_probe = true;
            continue;
        }
        const bool is_samples = argument == "--samples" && !have_samples;
        const bool is_interval = argument == "--interval" && !have_interval;
        if ((is_samples || is_interval) && index + 1 < argc) {
            unsigned value = 0;
            if (parse_unsigned(argv[++index], is_samples ? 1U : 50U,
                               is_samples ? 10000U : 60000U, value)) {
                if (is_samples) { samples = value; have_samples = true; }
                else { interval = value; have_interval = true; }
                continue;
            }
        }
        std::cerr << "Invalid probe argument: " << argument << '\n';
        usage(std::cerr);
        return 2;
    }
    try {
        return keyboard_requested ? keyboard_probe(samples, interval) : probe(samples, interval);
    } catch (const std::exception &error) {
        sensors::Snapshot snapshot;
        snapshot.error = error.what();
        snapshot.timestamp = sensors::Clock::now();
        print_snapshot(snapshot);
        return 1;
    }
}
