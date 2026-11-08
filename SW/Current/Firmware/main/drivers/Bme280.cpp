#include "Bme280.hpp"

#include <cmath>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace minilora {

namespace {
const char *TAG = "bme280";

constexpr uint8_t REG_ID = 0xD0;
constexpr uint8_t REG_RESET = 0xE0;
constexpr uint8_t REG_STATUS = 0xF3; // bit 3 = measuring
constexpr uint8_t REG_CTRL_HUM = 0xF2;
constexpr uint8_t REG_CTRL_MEAS = 0xF4;
constexpr uint8_t REG_CONFIG = 0xF5; // bits 2-4: IIR filter
constexpr uint8_t REG_DATA = 0xF7; // 8 bytes: P msb/lsb/xlsb, T, H

bool writeN(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len)
{
    return i2c_master_transmit(dev, buf, len, 100) == ESP_OK;
}
} // namespace

uint8_t Bme280::readReg(uint8_t reg, bool &ok)
{
    uint8_t val = 0;
    ok = i2c_master_transmit_receive(dev_, &reg, 1, &val, 1, 100) == ESP_OK;
    return val;
}

bool Bme280::writeReg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return writeN(dev_, buf, 2);
}

bool Bme280::readCalibration()
{
    bool ok;
    uint8_t buf[26];
    const uint8_t r1 = 0x88;
    ok = i2c_master_transmit_receive(dev_, &r1, 1, buf, 26, 100) == ESP_OK;
    if (!ok) {
        return false;
    }
    t1_ = buf[0] | (buf[1] << 8);
    t2_ = buf[2] | (buf[3] << 8);
    t3_ = buf[4] | (buf[5] << 8);
    p1_ = buf[6] | (buf[7] << 8);
    p2_ = buf[8] | (buf[9] << 8);
    p3_ = buf[10] | (buf[11] << 8);
    p4_ = buf[12] | (buf[13] << 8);
    p5_ = buf[14] | (buf[15] << 8);
    p6_ = buf[16] | (buf[17] << 8);
    p7_ = buf[18] | (buf[19] << 8);
    p8_ = buf[20] | (buf[21] << 8);
    p9_ = buf[22] | (buf[23] << 8);
    h1_ = buf[25];

    uint8_t hb[7];
    const uint8_t r2 = 0xE1;
    ok = i2c_master_transmit_receive(dev_, &r2, 1, hb, 7, 100) == ESP_OK;
    if (!ok) {
        return false;
    }
    h2_ = hb[0] | (hb[1] << 8);
    h3_ = hb[2];
    // H4/H5 are *signed* 12-bit values split across register pairs —
    // without the sign extension a negative trim silently corrupts the
    // humidity math.
    h4_ = (hb[3] << 4) | (hb[4] & 0x0F);
    if (h4_ & 0x800) {
        h4_ -= 4096;
    }
    h5_ = (hb[5] << 4) | (hb[4] >> 4);
    if (h5_ & 0x800) {
        h5_ -= 4096;
    }
    h6_ = static_cast<int8_t>(hb[6]);
    calibrated_ = true;
    return true;
}

bool Bme280::init()
{
    bool ok;
    const uint8_t id = readReg(REG_ID, ok);
    if (!ok || id != 0x60) {
        ESP_LOGE(TAG, "probe failed (id=0x%02x)", id);
        return false;
    }
    const uint8_t reset[2] = {REG_RESET, 0xB6};
    writeN(dev_, reset, 2);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!readCalibration()) {
        return false;
    }
    return true;
}

float Bme280::compensateTemp(int32_t adc) const
{
    const float v1 = ((float)adc / 16384.0f - (float)t1_ / 1024.0f) * (float)t2_;
    const float v2 = (((float)adc / 131072.0f - (float)t1_ / 8192.0f) *
                      ((float)adc / 131072.0f - (float)t1_ / 8192.0f)) * (float)t3_;
    tFine_ = (int32_t)(v1 + v2); // mutable through const_cast at call site
    return (v1 + v2) / 5120.0f;
}

float Bme280::compensatePressure(int32_t adc) const
{
    const float adcF = (float)adc;
    float var1 = ((float)tFine_ / 2.0f) - 64000.0f;
    float var2 = var1 * var1 * (float)p6_ / 32768.0f;
    var2 = var2 + var1 * (float)p5_ * 2.0f;
    var2 = var2 / 4.0f + ((float)p4_) * 65536.0f;
    var1 = (((float)p3_ * var1 * var1 / 524288.0f) + ((float)p2_ * var1)) / 524288.0f;
    // Bosch datasheet §8.2: (1 + var1/32768) * P1. The old /2 here was a
    // transcription error — bench 2026-11-01: pressure collapsed to ~1 hPa.
    var1 = (1.0f + var1 / 32768.0f) * (float)p1_;
    if (var1 <= 0.0f) {
        return NAN;
    }
    float p = 1048576.0f - adcF;
    p = (p - (var2 / 4096.0f)) * 6250.0f / var1;
    var1 = ((float)p9_) * p * p / 2147483648.0f;
    var2 = p * ((float)p8_) / 32768.0f;
    p = p + (var1 + var2 + ((float)p7_)) / 16.0f;
    return p;
}

float Bme280::compensateHum(int32_t adc) const
{
    // Bosch BME280 datasheet §8.2.3 (double form, kept in float — matches
    // the official BME280_driver compensate_humidity). The previous hybrid
    // formula produced garbage that saturated the payload's uint8 to 255%.
    const float h = (float)tFine_ - 76800.0f;
    const float var2 = (float)h4_ * 64.0f + ((float)h5_ / 16384.0f) * h;
    const float var3 = (float)adc - var2;
    const float var4 = (float)h2_ / 65536.0f;
    const float var5 = 1.0f + ((float)h3_ / 67108864.0f) * h;
    float var6 = 1.0f + ((float)h6_ / 67108864.0f) * h * var5;
    var6 = var3 * var4 * (var5 * var6);
    float hum = var6 * (1.0f - (float)h1_ * var6 / 524288.0f);
    if (hum > 100.0f) {
        hum = 100.0f;
    }
    if (hum < 0.0f) {
        hum = 0.0f;
    }
    return hum;
}

bool Bme280::read(Bme280Reading &out)
{
    if (!calibrated_) {
        return false;
    }
    // Humidity oversampling must be written before ctrl_meas (datasheet 5.4.3)
    if (!writeReg(REG_CTRL_HUM, 0x05) ||      // osrs_h = x16 (0b101)
        !writeReg(REG_CONFIG, 0x0C) ||       // IIR x8 (filter bits 011; t_sb
                                             // ignored in forced mode)
        !writeReg(REG_CTRL_MEAS, 0xB5)) {     // osrs_t=x16, osrs_p=x16, forced
        return false;
    }
    // x8/x8/x8 takes ~58 ms to measure; a blind 60 ms wait raced the
    // conversion and returned the reset values (bench 2026-11-01: 11 °C /
    // 255 % / 1 hPa). Poll STATUS.measuring (bit 3) until it clears.
    vTaskDelay(pdMS_TO_TICKS(2));
    bool sawBusy = false;
    for (int i = 0; i < 60; ++i) { // 2 ms + 60×2 ms ≈ 120 ms budget
        bool ok;
        const uint8_t st = readReg(REG_STATUS, ok);
        if (!ok) {
            return false;
        }
        if (st & 0x08) {
            sawBusy = true;
        } else if (sawBusy) {
            break; // measurement completed
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!sawBusy) {
        return false; // never started converting — data would be stale
    }

    uint8_t buf[8];
    const uint8_t reg = REG_DATA;
    if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 8, 100) != ESP_OK) {
        return false;
    }
    const int32_t adcP = (buf[0] << 12) | (buf[1] << 4) | (buf[2] >> 4);
    const int32_t adcT = (buf[3] << 12) | (buf[4] << 4) | (buf[5] >> 4);
    const int32_t adcH = (buf[6] << 8) | buf[7];

    // compensateTemp mutates tFine_ (mutable int32 state); keep order: T then P/H
    out.temperatureC = compensateTemp(adcT);
    out.pressurePa = compensatePressure(adcP);
    out.humidityPct = compensateHum(adcH);
    // Plausibility gate: stale/garbled register reads must not masquerade
    // as telemetry — report failure instead (payload falls back to 0s).
    return std::isfinite(out.pressurePa) && std::isfinite(out.humidityPct) &&
           out.pressurePa > 30000.0f && out.pressurePa < 120000.0f &&
           out.temperatureC > -40.0f && out.temperatureC < 85.0f;
}

} // namespace minilora
