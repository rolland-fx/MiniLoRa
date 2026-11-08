#include "Opt3001.hpp"

#include <cmath>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace minilora {

namespace {
const char *TAG = "opt3001";
constexpr uint8_t REG_RESULT = 0x00;
constexpr uint8_t REG_CONFIG = 0x01;

// v1 values: 0xC410 = auto full-scale, 800 ms, continuous; 0xC010 = shutdown.
constexpr uint16_t CFG_CONTINUOUS = 0xC410;
constexpr uint16_t CFG_SHUTDOWN = 0xC010;

constexpr uint8_t kConversionDelayMs = 120;
} // namespace

bool Opt3001::writeConfig(uint16_t cfg)
{
    const uint8_t buf[3] = {REG_CONFIG,
                            static_cast<uint8_t>(cfg >> 8),
                            static_cast<uint8_t>(cfg & 0xFF)};
    return i2c_master_transmit(dev_, buf, 3, 100) == ESP_OK;
}

bool Opt3001::init()
{
    return writeConfig(CFG_CONTINUOUS);
}

bool Opt3001::read(float &lux)
{
    vTaskDelay(pdMS_TO_TICKS(kConversionDelayMs));
    const uint8_t reg = REG_RESULT;
    uint8_t buf[2];
    if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 2, 100) != ESP_OK) {
        return false;
    }
    const uint16_t raw = (buf[0] << 8) | buf[1];
    const uint16_t exp = raw >> 12;
    const uint16_t man = raw & 0x0FFF;
    lux = man * 0.01f * std::powf(2.0f, static_cast<float>(exp));
    return std::isfinite(lux);
}

void Opt3001::shutdown()
{
    writeConfig(CFG_SHUTDOWN);
}

} // namespace minilora
