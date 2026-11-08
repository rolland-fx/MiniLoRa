// MiniLoRa board pin map (XIAO ESP32-C6 carrier).
//
// Source of truth: v1 firmware (tag Before-Rework) + Seeed XIAO ESP32C6
// variant pins_arduino.h (SDA=22, SCL=23, LED_BUILTIN=15) + README pin table.
// Phase 3 hardware validation re-checks every pin against the schematic.

#pragma once

#include <soc/gpio_num.h>

namespace minilora::pins {

// SX1262 (SPI radio)
inline constexpr gpio_num_t kSX1262Cs    = GPIO_NUM_17;
inline constexpr gpio_num_t kSX1262Sck   = GPIO_NUM_19;
inline constexpr gpio_num_t kSX1262Mosi  = GPIO_NUM_18;
inline constexpr gpio_num_t kSX1262Miso  = GPIO_NUM_20;
inline constexpr gpio_num_t kSX1262Reset = GPIO_NUM_2;
inline constexpr gpio_num_t kSX1262Busy  = GPIO_NUM_1;
inline constexpr gpio_num_t kSX1262Dio1  = GPIO_NUM_16;

// Board control
inline constexpr gpio_num_t kGnssWake    = GPIO_NUM_0;  // PA1010D wake/weekdown
inline constexpr gpio_num_t kLedBlue     = GPIO_NUM_15; // active LOW (XIAO builtin)
inline constexpr gpio_num_t kLedGreen    = GPIO_NUM_6;  // GPIO6 / ext connector, active HIGH
inline constexpr gpio_num_t kBatEnable   = GPIO_NUM_21; // battery divider enable
inline constexpr gpio_num_t kButton2     = GPIO_NUM_14; // BP2 (awake-only: GPIO14
                                                        // is not RTC-capable, so it
                                                        // cannot wake deep sleep)
inline constexpr gpio_num_t kBatSense    = GPIO_NUM_5;  // Vbat/2 analog
inline constexpr gpio_num_t kAccInt      = GPIO_NUM_4;  // shared PPS_INT net (GPS PPS + LIS3DH INT1 + OPT3001 INT)
inline constexpr gpio_num_t kButtonWakeup = GPIO_NUM_7; // user button BP1, active LOW, EXT1 wake

// Wio-SX1262 RF-switch control: HIGH = receiver path enabled (module
// datasheet); the SX1262's internal DIO2 overrides for TX. Hold HIGH during
// the radio session, LOW when the radio is parked.
inline constexpr gpio_num_t kLoRaRfSw    = GPIO_NUM_3;

// External connector button (BP2+EIO2) — v1 read it for gpioStatus bit1.
// Verified on HW: it is a real external button, not an antenna switch.
inline constexpr gpio_num_t kButton2Legacy = GPIO_NUM_14;

// I2C addresses (README table + HW probe on the production board)
// NOTE: LIS3DH SDO/SA0 is tied GND on this PCB => 0x18 (v1 code used 0x19
// and the accelerometer never answered — confirmed by I2C scan).
inline constexpr uint8_t kAddrGnss   = 0x10;
inline constexpr uint8_t kAddrAcc    = 0x18;
inline constexpr uint8_t kAddrTemp   = 0x76;
inline constexpr uint8_t kAddrLum    = 0x47;

// I2C bus (all sensors + ext connector)
inline constexpr gpio_num_t kI2cSda = GPIO_NUM_22;
inline constexpr gpio_num_t kI2cScl = GPIO_NUM_23;

// Battery ADC: Vbat/2 divider, 12-bit + calibration (divider verified 100k/100k)
inline constexpr int kBatAdcAttenuationMv = 2; // Vbat = 2 × VADC

} // namespace minilora::pins
