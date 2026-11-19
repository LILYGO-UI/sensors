#pragma once

#include "pages/sensors/sensor_source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sensors {

enum class MicrophoneBoard { cm0, k230 };
#if LILYGO_UI_PLATFORM_K230
inline constexpr auto native_microphone_board = MicrophoneBoard::k230;
#else
inline constexpr auto native_microphone_board = MicrophoneBoard::cm0;
#endif
const char *board_microphone_label(MicrophoneBoard board = native_microphone_board);

struct MicrophoneLevels {
    double rms_dbfs;
    std::vector<double> waveform;
};

// S16 mono (K230) or channel 0 of interleaved stereo (CM0).
std::optional<MicrophoneLevels> calculate_microphone_levels(
    const std::int16_t *samples, std::size_t sample_count, unsigned channels = 2);

class MicrophoneSource {
public:
    explicit MicrophoneSource(SourcePaths paths = {}, MicrophoneBoard board = native_microphone_board);
    ~MicrophoneSource();
    std::vector<SensorReading> snapshot();
    void rescan();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sensors
