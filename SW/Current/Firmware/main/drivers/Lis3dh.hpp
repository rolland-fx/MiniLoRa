// LIS3DH accelerometer driver — replicates v1 config: 400 Hz normal mode,
// ±4 g, 12-bit left-justified readings.

#pragma once

#include <driver/i2c_master.h>

namespace minilora {

class Lis3dh {
public:
    explicit Lis3dh(i2c_master_dev_handle_t dev) : dev_(dev) {}

    bool init();
    /// Single burst read of X/Y/Z raw counts (±4 g ⇒ 1 g ≈ 2048 counts).
    bool read(int16_t out[3]);
    void powerDown();

    /// Asset-mode motion wake (P5): low-power 10 Hz, INT1 = OR-combined
    /// high-event on any axis, pad ACTIVE-LOW + open-drain (D4D_INT1, set
    /// in init()) so it can share the EXT1 ANY_LOW mask with the button.
    /// sens: 1 = 128 mg, 2 = 256 mg, 3 = 512 mg (INT1_THS LSB = 16 mg).
    bool configureMotionWake(int sens);

private:
    bool writeReg(uint8_t reg, uint8_t val);
    i2c_master_dev_handle_t dev_;
};

} // namespace minilora
