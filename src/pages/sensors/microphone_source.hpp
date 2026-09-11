#pragma once

#include "pages/sensors/sensor_source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sensors {

struct MicrophoneLevels {
    double rms_dbfs;
    std::vector<double> waveform;
};

// Samples are interleaved S16 stereo; the CM0 microphone is on channel 0 (left).
std::optional<MicrophoneLevels> calculate_microphone_levels(
    const std::int16_t *samples, std::size_t sample_count);

class MicrophoneSource {
public:
    explicit MicrophoneSource(SourcePaths paths = {});
    ~MicrophoneSource();
    std::vector<SensorReading> snapshot();
    void rescan();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sensors
