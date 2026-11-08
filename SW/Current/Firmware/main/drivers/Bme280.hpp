// BME280 forced-mode driver (I2C, own implementation — no external dep).
// Datasheet BST-BME280-DS002 compensation formulas (float variant).

#pragma once

#include <driver/i2c_master.h>

namespace minilora {

struct Bme280Reading {
    float temperatureC;
    float humidityPct;
    float pressurePa;
};

class Bme280 {
public:
    explicit Bme280(i2c_master_dev_handle_t dev) : dev_(dev) {}

    bool init();                    // probe + load calibration
    /// Forced-mode single measurement (~60 ms). Returns false on I2C error.
    bool read(Bme280Reading &out);

private:
    bool readCalibration();
    uint8_t readReg(uint8_t reg, bool &ok);
    bool writeReg(uint8_t reg, uint8_t val);

    i2c_master_dev_handle_t dev_;
    bool calibrated_ = false;

    // Temperature compensation
    uint16_t t1_, p1_;
    int16_t  t2_, t3_, p2_, p3_, p4_, p5_, p6_, p7_, p8_, p9_;
    uint8_t  h1_, h3_;
    int16_t  h2_, h4_, h5_, h6_;

    mutable int32_t tFine_ = 0;
    float compensateTemp(int32_t adc) const;
    float compensateHum(int32_t adc) const;
    float compensatePressure(int32_t adc) const;
};

} // namespace minilora
