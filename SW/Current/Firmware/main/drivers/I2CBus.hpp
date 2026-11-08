// Shared I2C master bus + device handles (ESP-IDF i2c_master API).

#pragma once

#include <driver/i2c_master.h>
#include <soc/gpio_num.h>

namespace minilora {

class I2CBus {
public:
    /// Initialize the bus on (sda, scl) at 100 kHz (v1 Wire default).
    /// Idempotent: repeated calls return the existing bus.
    static I2CBus &instance();

    bool init(gpio_num_t sda, gpio_num_t scl, uint32_t hz = 100'000);
    void deinit();

    /// Register a device; returns nullptr on error.
    i2c_master_dev_handle_t addDevice(uint8_t address, uint32_t hz = 100'000);

    i2c_master_bus_handle_t handle() const { return bus_; }

private:
    I2CBus() = default;
    i2c_master_bus_handle_t bus_ = nullptr;
    bool initialized_ = false;
};

} // namespace minilora
