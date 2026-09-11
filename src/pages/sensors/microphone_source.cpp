#include "pages/sensors/microphone_source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>

#ifdef __linux__
#include <alsa/asoundlib.h>
#endif

namespace sensors {
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr unsigned capture_channels = 2;
constexpr unsigned microphone_channel = 0;
#ifdef __linux__
constexpr unsigned sample_rate = 48000;
constexpr std::size_t window_frames = sample_rate / 10;
#endif
constexpr std::size_t waveform_points = 240;
constexpr auto stale_after = 500ms;

double dbfs(double amplitude)
{
    return amplitude > 0 ? std::clamp(20 * std::log10(amplitude), -96.0, 0.0) : -96.0;
}

struct CaptureDevice {
    std::string alsa;
    std::string label = "Microphone (ES8389)";
    std::string identity = "alsa:es8389/left";
    std::string node;
    std::string error;
};

std::vector<fs::path> sound_entries(const fs::path &directory, std::string &error)
{
    std::vector<fs::path> result;
    std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    while (!ec && it != end) {
        result.push_back(it->path());
        it.increment(ec);
    }
    if (ec && ec != std::errc::no_such_file_or_directory)
        error = "Sound device discovery failed: " + ec.message();
    std::sort(result.begin(), result.end());
    return result;
}

CaptureDevice discover_device(const SourcePaths &paths, const std::string &override_device)
{
    CaptureDevice result;
    if (!override_device.empty()) {
        result.alsa = override_device;
        result.label = "Microphone (ALSA)";
        result.identity = "alsa:" + override_device + "/left";
        return result;
    }

    const auto entries = sound_entries(paths.sys / "class/sound", result.error);
    const std::regex card_pattern("card([0-9]+)");
    const std::regex capture_pattern("pcmC([0-9]+)D([0-9]+)c");
    bool found_card = false;
    for (const auto &card : entries) {
        std::smatch card_match;
        const auto card_name = card.filename().string();
        if (!std::regex_match(card_name, card_match, card_pattern)) continue;
        std::ifstream input(card / "id");
        std::string identifier;
        std::getline(input, identifier);
        std::transform(identifier.begin(), identifier.end(), identifier.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (identifier.find("es8389") == std::string::npos) continue;
        found_card = true;
        for (const auto &capture : entries) {
            std::smatch capture_match;
            const auto capture_name = capture.filename().string();
            if (!std::regex_match(capture_name, capture_match, capture_pattern) ||
                capture_match[1].str() != card_match[1].str()) continue;
            result.alsa = "plughw:" + card_match[1].str() + "," + capture_match[2].str();
            result.identity = "alsa:" + result.alsa + "/left";
            result.node = (paths.dev / "snd" / capture_name).string();
            std::error_code ec;
            if (!fs::exists(result.node, ec))
                result.error = "Microphone capture node unavailable: " + result.node;
            return result;
        }
    }
    if (result.error.empty())
        result.error = found_card ? "ES8389 sound card has no capture device" :
                                   "ES8389 microphone sound card not found";
    return result;
}

std::vector<SensorReading> readings_for(const CaptureDevice &device,
                                      const std::string &error,
                                      const std::optional<MicrophoneLevels> &levels = std::nullopt)
{
    const std::array<const char *, 2> names{"Raw waveform", "RMS level"};
    const std::array<const char *, 2> channel_names{"Waveform", "RMS"};
    const std::array<const char *, 2> keys{"waveform", "rms"};
    const std::array<const char *, 2> units{"FS", "dBFS"};
    const std::array<double, 2> values{0, levels ? levels->rms_dbfs : 0};
    const auto timestamp = Clock::now();
    std::vector<SensorReading> result;
    for (std::size_t index = 0; index < names.size(); ++index) {
        SensorReading reading;
        reading.id = device.identity + "/" + keys[index];
        reading.name = names[index];
        reading.category = "microphone";
        reading.device = device.label;
        reading.device_id = device.identity;
        reading.location = "system";
        reading.path = "ALSA " + (device.alsa.empty() ? std::string("ES8389 (not found)") : device.alsa) +
                       "; 48000 Hz S16_LE stereo; left channel; 100 ms";
        if (!device.node.empty()) reading.path += "; " + device.node;
        reading.available = levels.has_value() && error.empty();
        reading.error = error;
        reading.timestamp = timestamp;
        if (index == 0) {
            if (levels) reading.waveform = levels->waveform;
        } else {
            reading.channels.push_back({channel_names[index], units[index], values[index]});
        }
        result.push_back(std::move(reading));
    }
    return result;
}

#ifdef __linux__
struct PcmHandle {
    snd_pcm_t *value = nullptr;
    ~PcmHandle()
    {
        if (value) {
            snd_pcm_drop(value);
            snd_pcm_close(value);
        }
    }
};

struct CaptureGain {
    snd_mixer_t *mixer = nullptr;
    snd_mixer_elem_t *elements[2]{};
    long original[2]{};
    bool changed[2]{};

    ~CaptureGain()
    {
        for (unsigned index = 0; index < 2; ++index) {
            if (changed[index])
                snd_mixer_selem_set_capture_volume_all(elements[index], original[index]);
        }
        if (mixer) snd_mixer_close(mixer);
    }
};

unsigned configured_gain_db()
{
    constexpr unsigned default_gain = 18;
    const auto *value = std::getenv("LILYGO_SENSORS_MIC_GAIN_DB");
    if (!value || !*value) return default_gain;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end || parsed < 0 || parsed > 42) return default_gain;
    return static_cast<unsigned>(parsed);
}

void set_capture_gain(const std::string &device, CaptureGain &gain)
{
    const auto separator = device.find(':');
    if (separator == std::string::npos) return;
    const auto comma = device.find(',', separator + 1);
    const auto card = device.substr(separator + 1, comma == std::string::npos
                                                   ? std::string::npos : comma - separator - 1);
    if (card.empty()) return;
    const std::string mixer_name = "hw:" + card;
    if (snd_mixer_open(&gain.mixer, 0) < 0 ||
        snd_mixer_attach(gain.mixer, mixer_name.c_str()) < 0 ||
        snd_mixer_selem_register(gain.mixer, nullptr, nullptr) < 0 ||
        snd_mixer_load(gain.mixer) < 0) {
        if (gain.mixer) snd_mixer_close(gain.mixer);
        gain.mixer = nullptr;
        return;
    }
    const char *names[] = {"ADCL PGA", "ADCR PGA"};
    const long gain_step = static_cast<long>((configured_gain_db() + 1) / 3);
    for (snd_mixer_elem_t *element = snd_mixer_first_elem(gain.mixer); element;
         element = snd_mixer_elem_next(element)) {
        const char *name = snd_mixer_selem_get_name(element);
        for (unsigned index = 0; index < 2; ++index) {
            const std::size_t name_length = std::strlen(names[index]);
            if (!name || std::strncmp(name, names[index], name_length) != 0 ||
                (name[name_length] != '\0' && name[name_length] != ' ') ||
                !snd_mixer_selem_has_capture_volume(element) ||
                snd_mixer_selem_get_capture_volume(element, SND_MIXER_SCHN_FRONT_LEFT,
                                                   &gain.original[index]) < 0)
                continue;
            if (snd_mixer_selem_set_capture_volume_all(element, gain_step) >= 0) {
                gain.elements[index] = element;
                gain.changed[index] = true;
            }
        }
    }
}

std::string alsa_error(const char *operation, int error)
{
    if (error == -EBUSY) return "Microphone is in use by another application";
    if (error == -ENODEV || error == -ENXIO) return "Microphone disconnected or unavailable";
    if (error == -EPIPE) return "Microphone capture overrun; restarting capture";
    if (error == -ESTRPIPE) return "Microphone capture suspended; restarting capture";
    return std::string("Microphone ") + operation + ": " + snd_strerror(error);
}
#endif

}  // namespace

std::optional<MicrophoneLevels> calculate_microphone_levels(
    const std::int16_t *samples, std::size_t sample_count)
{
    if (!samples || sample_count == 0 || sample_count % capture_channels != 0) return std::nullopt;
    double sum_squares = 0;
    const auto frames = sample_count / capture_channels;
    std::vector<double> waveform;
    waveform.reserve(waveform_points);
    for (std::size_t index = microphone_channel; index < sample_count; index += capture_channels) {
        const int magnitude = std::abs(static_cast<int>(samples[index]));
        const double amplitude = magnitude / 32768.0;
        sum_squares += amplitude * amplitude;
    }
    for (std::size_t point = 0; point < waveform_points; ++point) {
        const auto frame = std::min(frames - 1, point * frames / waveform_points);
        waveform.push_back(static_cast<double>(samples[frame * capture_channels + microphone_channel]) / 32768.0);
    }
    return MicrophoneLevels{dbfs(std::sqrt(sum_squares / frames)), std::move(waveform)};
}

class MicrophoneSource::Impl {
public:
    explicit Impl(SourcePaths paths) : paths_(std::move(paths))
    {
        if (const auto *value = std::getenv("LILYGO_SENSORS_MIC_DEVICE")) override_device_ = value;
        readings_ = readings_for(CaptureDevice{}, "Microphone discovery pending");
        worker_ = std::thread([this] { run(); });
    }

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }

    std::vector<SensorReading> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = readings_;
        for (auto &reading : result) {
            if (reading.available && Clock::now() - reading.timestamp > stale_after) {
                reading.available = false;
                for (auto &channel : reading.channels) channel.value = 0;
                reading.error = "Microphone sample is stale; waiting for capture";
            }
        }
        return result;
    }

    void rescan()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reopen_ = true;
        }
        wake_.notify_all();
    }

private:
    void publish(const CaptureDevice &device, const std::string &error,
                 const std::optional<MicrophoneLevels> &levels = std::nullopt)
    {
        auto readings = readings_for(device, error, levels);
        std::lock_guard<std::mutex> lock(mutex_);
        readings_ = std::move(readings);
    }

    bool interrupted()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_ || reopen_;
    }

    void wait_for(std::chrono::milliseconds duration)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, duration, [this] { return stopping_ || reopen_; });
    }

#ifdef __linux__
    std::string capture(const CaptureDevice &device)
    {
        PcmHandle pcm;
        CaptureGain gain;
        int status = snd_pcm_open(&pcm.value, device.alsa.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (status < 0) return alsa_error("open failed", status);
        set_capture_gain(device.alsa, gain);
        status = snd_pcm_set_params(pcm.value, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                    capture_channels, sample_rate, 1, 200000);
        if (status < 0) return alsa_error("48 kHz stereo setup failed", status);
        status = snd_pcm_prepare(pcm.value);
        if (status < 0) return alsa_error("prepare failed", status);
        status = snd_pcm_start(pcm.value);
        if (status < 0) return alsa_error("start failed", status);

        std::array<std::int16_t, window_frames * capture_channels> samples{};
        std::size_t collected = 0;
        auto last_window = Clock::now();
        while (!interrupted()) {
            if (Clock::now() - last_window > stale_after)
                return "Microphone capture timed out: no complete 100 ms window";
            const auto count = snd_pcm_readi(pcm.value, samples.data() + collected * capture_channels,
                                            window_frames - collected);
            if (count == 0 || count == -EAGAIN || count == -EINTR) {
                wait_for(5ms);
                continue;
            }
            if (count < 0) return alsa_error("read failed", static_cast<int>(count));
            if (static_cast<std::size_t>(count) > window_frames - collected)
                return "Microphone returned an invalid capture frame count";
            collected += static_cast<std::size_t>(count);
            if (collected == window_frames) {
                publish(device, {}, calculate_microphone_levels(samples.data(), samples.size()));
                collected = 0;
                last_window = Clock::now();
            }
        }
        return {};
    }
#endif

    void run()
    {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) return;
                reopen_ = false;
            }
            CaptureDevice device;
            std::string error;
            try {
                device = discover_device(paths_, override_device_);
                error = device.error;
                if (error.empty() && (paths_.sys != fs::path("/sys") || paths_.dev != fs::path("/dev")))
                    error = "Microphone hardware capture disabled for overridden sys/dev roots";
                if (error.empty()) {
#ifdef __linux__
                    publish(device, "Microphone initializing; waiting for a complete sample window");
                    error = capture(device);
#else
                    error = "Microphone capture requires Linux ALSA";
#endif
                }
            } catch (const std::exception &failure) {
                error = std::string("Microphone capture failed: ") + failure.what();
            }
            if (!error.empty()) publish(device, error);
            wait_for(1s);
        }
    }

    SourcePaths paths_;
    std::string override_device_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool reopen_ = false;
    std::vector<SensorReading> readings_;
    std::thread worker_;
};

MicrophoneSource::MicrophoneSource(SourcePaths paths) : impl_(std::make_unique<Impl>(std::move(paths))) {}
MicrophoneSource::~MicrophoneSource() = default;
std::vector<SensorReading> MicrophoneSource::snapshot() { return impl_->snapshot(); }
void MicrophoneSource::rescan() { impl_->rescan(); }

}  // namespace sensors
