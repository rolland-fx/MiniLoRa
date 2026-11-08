#include "Sleep.hpp"

#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>

#include "../drivers/Pins.hpp"

namespace minilora {

namespace {
constexpr uint64_t kWakeupPinMask = 1ULL << 7; // GPIO7, active LOW (v1)
constexpr uint64_t kMotionPinMask = 1ULL << 4; // GPIO4, LIS3DH INT1 (P5)
}

void prepareSleep(uint32_t sleepMs)
{
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepMs) * 1000ULL);
    esp_sleep_enable_ext1_wakeup(kWakeupPinMask, ESP_EXT1_WAKEUP_ANY_LOW);
    rtc_gpio_pulldown_dis(GPIO_NUM_7);
    rtc_gpio_pullup_dis(GPIO_NUM_7);

    // LED sleep states — v1 parity (applyGpio6State else-branch drove
    // GPIO6 HIGH before sleeping). Our old LOW kept the green LED lit all
    // night on the production board (its green is active-low). C6 note:
    // gpio_hold_en alone holds digital pads (GPIO15) through deep sleep
    // (SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP) — no global API needed.
    gpio_set_level(pins::kLedGreen, 1);
    gpio_set_level(pins::kLedBlue, 1);

    // Hold outputs through deep sleep (v1 prepareSleep)
    gpio_hold_en(pins::kGnssWake);
    gpio_hold_en(pins::kSX1262Reset);
    gpio_hold_en(pins::kLedBlue);
    gpio_hold_en(pins::kLedGreen);
    gpio_hold_en(pins::kBatEnable);

    // Isolate the RTC-domain pins (v1 parity; GPIO7 stays as wake source)
    rtc_gpio_isolate(GPIO_NUM_0);
    rtc_gpio_isolate(GPIO_NUM_1);
    rtc_gpio_isolate(GPIO_NUM_4);
    rtc_gpio_isolate(GPIO_NUM_5);

    esp_deep_sleep_start(); // never returns
}

void prepareSleepMotion(uint32_t sleepMs)
{
    // GPIO4 is RTC-capable: input + pull-up (the shared PPS_INT net has no
    // external pull-up; INT1 is active-low open-drain). NOT isolated.
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepMs) * 1000ULL);
    esp_sleep_enable_ext1_wakeup(kWakeupPinMask | kMotionPinMask,
                                 ESP_EXT1_WAKEUP_ANY_LOW);
    rtc_gpio_pulldown_dis(GPIO_NUM_7);
    rtc_gpio_pullup_dis(GPIO_NUM_7);
    rtc_gpio_set_direction(GPIO_NUM_4, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(GPIO_NUM_4);
    rtc_gpio_pulldown_dis(GPIO_NUM_4);
    rtc_gpio_hold_en(GPIO_NUM_4);

    gpio_set_level(pins::kLedGreen, 1);
    gpio_set_level(pins::kLedBlue, 1);

    gpio_hold_en(pins::kGnssWake);
    gpio_hold_en(pins::kSX1262Reset);
    gpio_hold_en(pins::kLedBlue);
    gpio_hold_en(pins::kLedGreen);
    gpio_hold_en(pins::kBatEnable);

    rtc_gpio_isolate(GPIO_NUM_0);
    rtc_gpio_isolate(GPIO_NUM_1);
    rtc_gpio_isolate(GPIO_NUM_5);

    esp_deep_sleep_start(); // never returns
}

void feedWatchdog()
{
    esp_task_wdt_reset();
}

} // namespace minilora
