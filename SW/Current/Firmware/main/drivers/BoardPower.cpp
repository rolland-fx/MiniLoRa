#include "BoardPower.hpp"
#include "Pins.hpp"

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace minilora {

namespace {
const char *TAG = "power";

adc_oneshot_unit_handle_t g_adc = nullptr;
adc_cali_handle_t g_cali = nullptr;
}

BoardPower &BoardPower::instance()
{
    static BoardPower p;
    return p;
}

void BoardPower::init()
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << pins::kGnssWake) | (1ULL << pins::kLedBlue) |
                        (1ULL << pins::kLedGreen) | (1ULL << pins::kBatEnable) |
                        (1ULL << pins::kSX1262Reset),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);

    // BP2 (GPIO14): input with pull-up — R18 is fitted on the board, the
    // internal pull-up is a belt-and-braces backup. Awake-only button:
    // GPIO14 is not RTC-capable, so it cannot wake deep sleep.
    // BP1 (GPIO7): input too — R4 pulls it up externally. Without this the
    // pad keeps its sleep-time RTC config after an EXT1 wake and the
    // digital input buffer stays disabled: gpio_get_level() then returns 0
    // forever and longPressDetected() counted a phantom 5-s hold — EVERY
    // button wake (short press included) opened the config window
    // (bench 2026-11-01).
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << pins::kButton2) | (1ULL << pins::kButtonWakeup),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    // Release deep-sleep holds (v1 prepareSleep held these pins)
    gpio_hold_dis(pins::kLedBlue);
    gpio_hold_dis(pins::kLedGreen);
    gpio_hold_dis(pins::kGnssWake);
    gpio_hold_dis(pins::kBatEnable);
    gpio_hold_dis(pins::kSX1262Reset);

    gpio_set_level(pins::kLedBlue, 1);     // off (active low)
    ledGreen(false);
    gpio_set_level(pins::kBatEnable, 0);
    gpio_set_level(pins::kGnssWake, 0);
}

void BoardPower::ledBlue(bool on)
{
    gpio_set_level(pins::kLedBlue, on ? 0 : 1);
}

void BoardPower::ledGreen(bool on)
{
    greenOn_ = on;
    // Active-low on the production board (verified on the bench: the
    // sleep-path fix drives this pin HIGH for OFF). Same polarity as blue.
    gpio_set_level(pins::kLedGreen, on ? 0 : 1);
}

uint16_t BoardPower::readBatteryMv()
{
    static bool adcReady = false;
    if (!adcReady) {
        const adc_oneshot_unit_init_cfg_t cfg = {
            .unit_id = ADC_UNIT_1,
            .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        adc_oneshot_new_unit(&cfg, &g_adc);
        adc_oneshot_chan_cfg_t chan = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        adc_oneshot_config_channel(g_adc, ADC_CHANNEL_5, &chan); // GPIO5
        const adc_cali_curve_fitting_config_t cali = {
            .unit_id = ADC_UNIT_1,
            .chan = ADC_CHANNEL_5,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali, &g_cali) != ESP_OK) {
            g_cali = nullptr;
        }
        adcReady = true;
    }

    gpio_set_level(pins::kBatEnable, 1);
    // RC settle: divider is R2=100k/R3=100k with C7=1uF on the ADC node →
    // Thevenin 50k x 1uF = tau 50 ms. 100 ms (2 tau) biased the 8-sample
    // average ~100 mV low and made it depend on the node's leftover state
    // (the observed 134 mV jump between two reads 264 ms apart). v1 used
    // 200 ms + 100 ms inter-sample delays; 400 ms = 8 tau is comfortably
    // settled (costs ~1 s of the awake window — negligible).
    vTaskDelay(pdMS_TO_TICKS(400));

    uint32_t sumMv = 0;
    for (int i = 0; i < 8; ++i) {
        int raw = 0;
        adc_oneshot_read(g_adc, ADC_CHANNEL_5, &raw);
        int mv = raw;
        if (g_cali) {
            adc_cali_raw_to_voltage(g_cali, raw, &mv);
        }
        sumMv += static_cast<uint32_t>(mv) * pins::kBatAdcAttenuationMv;
        vTaskDelay(pdMS_TO_TICKS(100)); // v1 parity inter-sample delay
    }
    gpio_set_level(pins::kBatEnable, 0);
    return static_cast<uint16_t>(sumMv / 8);
}

} // namespace minilora
