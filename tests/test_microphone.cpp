#include "pages/sensors/microphone_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace sensors;
using namespace std::chrono_literals;

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, double tolerance, const std::string &message)
{
    require(std::isfinite(actual) && std::abs(actual - expected) < tolerance,
            message + ": got " + std::to_string(actual));
}

struct Environment {
    std::string key = "LILYGO_SENSORS_MIC_DEVICE";
    std::string previous;
    bool present;

    explicit Environment(const char *value = nullptr)
    {
        const auto *old = std::getenv(key.c_str());
        present = old != nullptr;
        if (old) previous = old;
        if (value) ::setenv(key.c_str(), value, 1);
        else ::unsetenv(key.c_str());
    }
    ~Environment()
    {
        if (present) ::setenv(key.c_str(), previous.c_str(), 1);
        else ::unsetenv(key.c_str());
    }
};

struct Fixture {
    fs::path root;
    SourcePaths paths;

    Fixture()
    {
        std::string pattern = (fs::temp_directory_path() / "lilygo-microphone-XXXXXX").string();
        const auto *created = ::mkdtemp(pattern.data());
        require(created != nullptr, "create microphone fixture");
        root = created;
        paths = {root / "sys", root / "dev", false};
        fs::create_directories(paths.sys / "class/sound");
        fs::create_directories(paths.dev / "snd");
    }

    ~Fixture() { std::error_code error; fs::remove_all(root, error); }

    void card(unsigned number, const std::string &identifier)
    {
        const auto directory = paths.sys / "class/sound" / ("card" + std::to_string(number));
        fs::create_directories(directory);
        std::ofstream(directory / "id") << identifier << '\n';
    }

    void pcm(unsigned card, unsigned device, char direction, bool with_node = true)
    {
        const auto name = "pcmC" + std::to_string(card) + "D" + std::to_string(device) + direction;
        fs::create_directories(paths.sys / "class/sound" / name);
        if (with_node) std::ofstream(paths.dev / "snd" / name) << "";
    }
};

std::vector<SensorReading> wait_for(MicrophoneSource &source,
                                   const std::function<bool(const SensorReading &)> &condition,
                                   const std::string &message)
{
    const auto deadline = Clock::now() + 2s;
    while (Clock::now() < deadline) {
        auto snapshot = source.snapshot();
        if (snapshot.size() == 2 && condition(snapshot.front())) return snapshot;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error(message);
}

void require_unavailable(const std::vector<SensorReading> &readings)
{
    require(readings.size() == 2, "microphone always has two measurement entries");
    const std::vector<std::string> names{"Raw waveform", "RMS level"};
    const std::vector<std::string> channel_names{"", "RMS"};
    const std::vector<std::string> units{"", "dBFS"};
    for (std::size_t index = 0; index < readings.size(); ++index) {
        const auto &reading = readings[index];
        require(!reading.available && !reading.error.empty(), "unavailable microphone has explicit error");
        require(reading.category == "microphone" && reading.location == "system", "microphone category and location");
        require(reading.device_id == readings.front().device_id && !reading.device_id.empty(),
                "all microphone measurements share physical identity");
        require(reading.name == names[index], "unavailable measurement keeps its name");
        if (index == 0) {
            require(reading.channels.empty() && reading.waveform.empty(), "unavailable waveform has no samples");
        } else {
            require(reading.channels.size() == 1 && reading.channels.front().name == channel_names[index] &&
                        reading.channels.front().unit == units[index] && reading.channels.front().value == 0,
                    "unavailable RMS retains channel metadata");
        }
        require(reading.path.find("48000") != std::string::npos && reading.path.find("left") != std::string::npos &&
                reading.path.find("100 ms") != std::string::npos, "capture metadata includes rate, channel and window");
    }
    require(readings[0].id != readings[1].id, "each measurement has distinct identity");
}

void silence_and_left_channel()
{
    std::vector<std::int16_t> samples(4800 * 2, 0);
    auto levels = calculate_microphone_levels(samples.data(), samples.size());
    require(levels.has_value(), "silence is a valid microphone window");
    near(levels->rms_dbfs, -96, 0.000001, "silence RMS floor");
    require(levels->waveform.size() == 240, "silence returns a decimated raw waveform");
    for (std::size_t i = 1; i < samples.size(); i += 2) samples[i] = -32768;
    levels = calculate_microphone_levels(samples.data(), samples.size());
    near(levels->rms_dbfs, -96, 0.000001, "right channel does not affect microphone RMS");
    require(std::all_of(levels->waveform.begin(), levels->waveform.end(),
                        [](double value) { return value == 0; }), "right channel does not affect waveform");
}

void known_sine()
{
    std::vector<std::int16_t> samples(4800 * 2);
    const auto pi = std::acos(-1.0);
    for (std::size_t frame = 0; frame < samples.size() / 2; ++frame)
        samples[frame * 2] = static_cast<std::int16_t>(std::lround(16384 * std::sin(2 * pi * frame / 48)));
    const auto levels = calculate_microphone_levels(samples.data(), samples.size());
    require(levels.has_value(), "known sine is valid");
    near(levels->rms_dbfs, 20 * std::log10(0.5 / std::sqrt(2.0)), 0.001, "half-scale sine RMS");
    require(levels->waveform.size() == 240 &&
                std::any_of(levels->waveform.begin(), levels->waveform.end(),
                            [](double value) { return std::abs(value) > 0.4; }),
            "raw waveform preserves the left-channel sine");
}

void invalid_windows()
{
    const std::int16_t sample = 0;
    require(!calculate_microphone_levels(nullptr, 0), "empty input is unavailable");
    require(!calculate_microphone_levels(nullptr, 2), "null input is unavailable");
    require(!calculate_microphone_levels(&sample, 0), "zero frame window is unavailable");
    require(!calculate_microphone_levels(&sample, 1), "incomplete stereo frame is unavailable");
}

void discovery_and_rescan()
{
    Environment environment;
    Fixture fixture;
    fixture.card(0, "USB Audio");
    fixture.pcm(0, 0, 'c');
    fixture.card(3, "  BoardEs8389  ");
    fixture.pcm(3, 0, 'p');
    fixture.pcm(3, 1, 'c');
    MicrophoneSource source(fixture.paths);
    auto readings = wait_for(source, [](const SensorReading &reading) {
        return reading.error.find("disabled for overridden") != std::string::npos;
    }, "fixture microphone is discovered without hardware capture");
    require_unavailable(readings);
    require(readings[0].device == "Microphone (ES8389)" && readings[0].path.find("plughw:3,1") != std::string::npos,
            "only ES8389 capture PCM is selected, ignoring other cards and playback");
    require(readings[0].path.find((fixture.paths.dev / "snd/pcmC3D1c").string()) != std::string::npos,
            "microphone device path respects dev root override");
    const auto original_identity = readings[0].device_id;
    source.rescan();
    readings = wait_for(source, [](const SensorReading &reading) {
        return reading.error.find("disabled for overridden") != std::string::npos;
    }, "repeat rescan completes");
    require(readings[0].device_id == original_identity, "rescan keeps measurement identity stable");

    fs::remove(fixture.paths.sys / "class/sound/card3/id");
    source.rescan();
    readings = wait_for(source, [](const SensorReading &reading) {
        return reading.error.find("sound card not found") != std::string::npos;
    }, "rescan reports removed microphone");
    require_unavailable(readings);

    fixture.card(5, "es8389");
    fixture.pcm(5, 0, 'p');
    source.rescan();
    readings = wait_for(source, [](const SensorReading &reading) {
        return reading.error.find("no capture device") != std::string::npos;
    }, "playback-only ES8389 has explicit capture error");
    require_unavailable(readings);

    fixture.pcm(5, 2, 'c', false);
    source.rescan();
    readings = wait_for(source, [](const SensorReading &reading) {
        return reading.error.find("capture node unavailable") != std::string::npos;
    }, "missing microphone node has explicit error");
    require_unavailable(readings);
}

void override_roots_never_capture()
{
    Environment environment("null");
    Fixture fixture;
    for (const auto &paths : {fixture.paths, SourcePaths{fixture.paths.sys, "/dev", false},
                             SourcePaths{"/sys", fixture.paths.dev, false}}) {
        const auto before = Clock::now();
        {
            MicrophoneSource source(paths);
            auto readings = wait_for(source, [](const SensorReading &reading) {
                return reading.error.find("disabled for overridden") != std::string::npos;
            }, "either root override disables ALSA even with explicit device override");
            require_unavailable(readings);
            require(readings[0].device == "Microphone (ALSA)" && readings[0].path.find("ALSA null;") != std::string::npos,
                    "explicit ALSA override is exact and does not claim an ES8389");
        }
        require(Clock::now() - before < 500ms, "destructor interrupts retry wait promptly");
    }
}

}  // namespace

int main()
{
    try {
        silence_and_left_channel();
        known_sine();
        invalid_windows();
        discovery_and_rescan();
        override_roots_never_capture();
        std::cout << "microphone source tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
