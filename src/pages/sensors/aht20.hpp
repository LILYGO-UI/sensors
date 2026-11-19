#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sensors {

struct Aht20Sample {
    double temperature = 0;
    double humidity = 0;
    std::string error;
};

Aht20Sample decode_aht20(const std::array<std::uint8_t, 7> &frame);
#if defined(__linux__) && LILYGO_UI_PLATFORM_K230
Aht20Sample read_aht20(const std::filesystem::path &adapter);
#endif

}  // namespace sensors
