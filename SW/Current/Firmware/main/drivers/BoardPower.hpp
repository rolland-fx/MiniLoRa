// Board power / GPIO housekeeping: LED polarity, battery ADC, GPIO holds.

#pragma once

#include <soc/gpio_num.h>
#include <cstdint>

namespace minilora {

class BoardPower {
public:
    static BoardPower &instance();

    /// Configure output pins; releases any gpio_hold from deep sleep.
    void init();

    // LED_BLUE is active-LOW, LED_GREEN active-HIGH (v1 semantics)
    void ledBlue(bool on);
    void ledGreen(bool on);
    bool greenState() const { return greenOn_; }

    /// Enable divider, average 8 calibrated samples, disable divider.
    /// Blocks ~800 ms (v1 timing). Returns millivolts.
    uint16_t readBatteryMv();

private:
    BoardPower() = default;
    bool greenOn_ = false;
};

} // namespace minilora
