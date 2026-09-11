#pragma once

#include "pages/sensors/sensor_source.hpp"

#include <functional>

namespace sensors {

enum class AddressStatus { absent, acknowledged, busy, unsupported, failed };
struct AddressProbeResult {
    AddressStatus status;
    std::string error;
};

using AddressProbe = std::function<AddressProbeResult(int)>;

// Shared merge policy also allows testing without an I2C adapter.
void probe_i2c_addresses(BusInfo &bus, const AddressProbe &probe);
void probe_i2c_bus(BusInfo &bus, const std::filesystem::path &dev_root);

}  // namespace sensors
