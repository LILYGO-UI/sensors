#include "platform_paths.hpp"
#include "pages/sensors/aht20.hpp"

#if defined(__linux__) && LILYGO_UI_PLATFORM_K230
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#endif

namespace sensors {

Aht20Sample decode_aht20(const std::array<std::uint8_t, 7> &frame)
{
    std::uint8_t crc = 0xff;
    for (auto byte : frame) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80) ? 0x31 : 0);
    }
    if (crc != 0) return {0, 0, "AHT20 CRC mismatch"};
    if (frame[0] & 0x80) return {0, 0, "AHT20 measurement busy"};
    if (!(frame[0] & 0x08)) return {0, 0, "AHT20 calibration not ready"};
    const int humidity = (frame[1] << 12) | (frame[2] << 4) | (frame[3] >> 4);
    const int temperature = ((frame[3] & 0x0f) << 16) | (frame[4] << 8) | frame[5];
    return {temperature * 200.0 / 1048576 - 50, humidity * 100.0 / 1048576, {}};
}

#if defined(__linux__) && LILYGO_UI_PLATFORM_K230
Aht20Sample read_aht20(const std::filesystem::path &adapter)
{
    struct Descriptor {
        int fd;
        ~Descriptor() { if (fd >= 0) ::close(fd); }
    } bus{::open(adapter.c_str(), O_RDWR | O_CLOEXEC)};
    if (bus.fd < 0 || ::ioctl(bus.fd, I2C_SLAVE, 0x38) < 0)
        return {0, 0, adapter.string() + ": " + std::strerror(errno)};
    const std::uint8_t trigger[] = {0xac, 0x33, 0};
    const auto written = ::write(bus.fd, trigger, sizeof(trigger));
    if (written != sizeof(trigger))
        return {0, 0, written < 0 ? std::strerror(errno) : "Short AHT20 measurement command"};
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    std::array<std::uint8_t, 7> frame{};
    const auto count = ::read(bus.fd, frame.data(), frame.size());
    if (count != static_cast<ssize_t>(frame.size()))
        return {0, 0, count < 0 ? std::strerror(errno) : "Short AHT20 measurement frame"};
    return decode_aht20(frame);
}
#endif

}  // namespace sensors
