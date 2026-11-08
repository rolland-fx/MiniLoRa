#include "I2CBus.hpp"

#include <esp_log.h>

namespace minilora {

namespace {
const char *TAG = "i2c";
}

I2CBus &I2CBus::instance()
{
    static I2CBus bus;
    return bus;
}

bool I2CBus::init(gpio_num_t sda, gpio_num_t scl, uint32_t hz)
{
    if (initialized_) {
        return true;
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    if (i2c_new_master_bus(&cfg, &bus_) != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed");
        return false;
    }
    initialized_ = true;
    return true;
}

void I2CBus::deinit()
{
    if (initialized_) {
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
        initialized_ = false;
    }
}

i2c_master_dev_handle_t I2CBus::addDevice(uint8_t address, uint32_t hz)
{
    if (!initialized_) {
        return nullptr;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = hz,
    };
    i2c_master_dev_handle_t handle = nullptr;
    if (i2c_master_bus_add_device(bus_, &dev, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02x failed", address);
        return nullptr;
    }
    return handle;
}

} // namespace minilora
