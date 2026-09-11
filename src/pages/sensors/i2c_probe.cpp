#include "pages/sensors/i2c_probe.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>

#ifdef __linux__
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace sensors {
namespace {
void scan_error(BusInfo &bus, const std::string &error)
{
    if (!bus.error.empty()) bus.error += "; ";
    bus.error += "I2C scan: " + error;
}
}  // namespace

void probe_i2c_addresses(BusInfo &bus, const AddressProbe &probe)
{
    std::set<int> registered;
    for (const auto &device : bus.devices) {
        if (device.status == BusDeviceStatus::registered) registered.insert(device.address);
    }
    bus.devices.erase(std::remove_if(bus.devices.begin(), bus.devices.end(), [](const BusDevice &device) {
        return device.status != BusDeviceStatus::registered;
    }), bus.devices.end());
    bool unsupported = false;
    for (int address = 0x03; address <= 0x77; ++address) {
        if (registered.count(address)) continue;
        const auto result = probe(address);
        if (result.status == AddressStatus::acknowledged)
            bus.devices.push_back({address, "Unknown I2C device", "", BusDeviceStatus::acknowledged});
        else if (result.status == AddressStatus::busy)
            bus.devices.push_back({address, "I2C device", "", BusDeviceStatus::busy});
        else if (result.status == AddressStatus::unsupported) unsupported = true;
        else if (result.status == AddressStatus::failed) {
            scan_error(bus, result.error);
            break;
        }
    }
    if (unsupported) scan_error(bus, "adapter cannot probe some addresses");
    std::sort(bus.devices.begin(), bus.devices.end(), [](const BusDevice &a, const BusDevice &b) {
        return a.address < b.address;
    });
}

void probe_i2c_bus(BusInfo &bus, const std::filesystem::path &dev_root)
{
#ifdef __linux__
    const auto path = dev_root / ("i2c-" + std::to_string(bus.number));
    struct Descriptor {
        int value;
        ~Descriptor() { if (value >= 0) ::close(value); }
    } descriptor{::open(path.c_str(), O_RDWR | O_CLOEXEC)};
    if (descriptor.value < 0) {
        scan_error(bus, path.string() + ": " + std::strerror(errno));
        return;
    }
    unsigned long functions = 0;
    if (::ioctl(descriptor.value, I2C_FUNCS, &functions) < 0) {
        scan_error(bus, "adapter capabilities: " + std::string(std::strerror(errno)));
        return;
    }
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    probe_i2c_addresses(bus, [&](int address) -> AddressProbeResult {
        if (Clock::now() >= deadline) return {AddressStatus::failed, "time limit reached; scan incomplete"};
        // Match i2cdetect's default transaction choice for EEPROM-sensitive ranges.
        const bool read_byte = (address >= 0x30 && address <= 0x37) ||
                               (address >= 0x50 && address <= 0x5f) ||
                               !(functions & I2C_FUNC_SMBUS_QUICK);
        const auto required = read_byte ? I2C_FUNC_SMBUS_READ_BYTE : I2C_FUNC_SMBUS_QUICK;
        if (!(functions & required)) return {AddressStatus::unsupported, {}};
        if (::ioctl(descriptor.value, I2C_SLAVE, address) < 0) {
            if (errno == EBUSY) return {AddressStatus::busy, {}};
            return {AddressStatus::failed, std::strerror(errno)};
        }
        union i2c_smbus_data data{};
        struct i2c_smbus_ioctl_data arguments{};
        arguments.read_write = read_byte ? I2C_SMBUS_READ : I2C_SMBUS_WRITE;
        arguments.size = read_byte ? I2C_SMBUS_BYTE : I2C_SMBUS_QUICK;
        arguments.data = &data;
        if (::ioctl(descriptor.value, I2C_SMBUS, &arguments) == 0)
            return {AddressStatus::acknowledged, {}};
        const int error = errno;
        if (error == ENXIO || error == EREMOTEIO || error == EIO)
            return {AddressStatus::absent, {}};
        return {AddressStatus::failed, std::strerror(error)};
    });
#else
    (void)bus;
    (void)dev_root;
#endif
}
}  // namespace sensors
