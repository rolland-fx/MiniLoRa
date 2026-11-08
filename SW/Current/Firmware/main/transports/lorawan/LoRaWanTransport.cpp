#include "LoRaWanTransport.hpp"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <RadioLib.h>

#include <cstring>

#include "../../drivers/Pins.hpp"
#include "../../domain/Telemetry.hpp"

namespace minilora {

namespace {
const char *TAG = "lorawan";

// RTC-persistent OTAA state (survives deep sleep, lost on power cycle —
// same tradeoff as v1; a device that loses power rejoins once).
// Guarded by a magic tag: the buffer layout is RadioLib-version (and
// firmware-) dependent, so state written by an older build must be
// discarded — a stale nonces buffer carries a corrupt channel plan and
// made every join fail with ERR_NO_CHANNEL_AVAILABLE (bench 2026-11-01).
constexpr uint32_t kLoraStateMagic = 0x4D4C3236;  // "ML26" — bump on format change
RTC_DATA_ATTR uint32_t loraStateMagic = 0;
RTC_DATA_ATTR uint8_t loraNonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE] = {0};
RTC_DATA_ATTR uint8_t loraSession[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] = {0};
RTC_DATA_ATTR bool loraHasSession = false;

EspHal *hal = nullptr;
SX1262 *radio = nullptr;
LoRaWANNode *node = nullptr;

// The LoRaWANNode stores this pointer for its whole lifetime — the band
// MUST live at file scope. A stack local dangles after begin() returns
// and every band read in createSession() returns garbage (DR=7 instead
// of 3, RX2=863.071 MHz instead of 869.525, reverted channel masks →
// every join failed with ERR_NO_CHANNEL_AVAILABLE -1106; v1 avoided the
// same trap with a file-scope `const LoRaWANBand_t Region`).
static const LoRaWANBand_t kLoraBand = EU868;

void deinitPins()
{
    // Park the RF switch low (radio off — RX path disabled, DIO2 idle)
    gpio_set_level(pins::kLoRaRfSw, 0);
    gpio_reset_pin(pins::kLoRaRfSw);
    // Leave the radio bus low-power (v1 prepareSleep pattern)
    gpio_reset_pin(pins::kSX1262Cs);
    gpio_reset_pin(pins::kSX1262Sck);
    gpio_reset_pin(pins::kSX1262Mosi);
    gpio_reset_pin(pins::kSX1262Miso);
    gpio_reset_pin(pins::kSX1262Dio1);
}
} // namespace

bool LoRaWanTransport::begin(uint64_t joinEUI, uint64_t devEUI,
                             const uint8_t *nwkKey16, const uint8_t *appKey16)
{
    if (!hal) {
        hal = new EspHal(pins::kSX1262Sck, pins::kSX1262Miso, pins::kSX1262Mosi,
                         SPI2_HOST, 8'000'000);
    }
    if (!radio) {
        radio = new SX1262(new Module(hal, pins::kSX1262Cs, pins::kSX1262Dio1,
                                      pins::kSX1262Reset, pins::kSX1262Busy));
    }
    // v7.x positional args: freq(MHz), bw(kHz), sf, cr, syncWord, power(dBm),
    // preamble. The LoRaWAN node reconfigures all of these per channel — the
    // values only have to be *valid*: the old call passed (5, 0x12, 8)
    // meaning 5 MHz / 18 kHz -> RADIOLIB_ERR_INVALID_BANDWIDTH (-8) on
    // every boot, so no join was ever attempted (bench 2026-11-01).
    int16_t state = radio->begin(868.0, 125.0, 9, 5, 0x12, 14, 8);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "radio begin failed: %d", state);
        return false;
    }
    // The SX1262's DIO2 drives the module's internal RF switch for TX;
    // GPIO3 (module RF_SW input) must be HIGH for the RX path.
    radio->setDio2AsRfSwitch(true);
    gpio_set_direction(pins::kLoRaRfSw, GPIO_MODE_OUTPUT);
    gpio_hold_dis(pins::kLoRaRfSw);
    gpio_set_level(pins::kLoRaRfSw, 1);

    if (!node) {
        node = new LoRaWANNode(radio, &kLoraBand, 0);
    }
    // Discard state written by any other build (see kLoraStateMagic) —
    // a fresh OTAA join is always safe.
    if (loraStateMagic != kLoraStateMagic) {
        loraStateMagic = kLoraStateMagic;
        loraHasSession = false;
        memset(loraNonces, 0, sizeof(loraNonces));
        memset(loraSession, 0, sizeof(loraSession));
        ESP_LOGI(TAG, "stale/foreign LoRa state cleared — fresh join");
    }
    node->beginOTAA(joinEUI, devEUI, nwkKey16, appKey16);
    // ADR (P3): let the network adapt datarate/TX power over time — shorter
    // uplinks as the tracker settles near a gateway. Explicit (already the
    // RadioLib default).
    node->setADR(true);

    if (loraHasSession &&
        node->setBufferNonces(loraNonces) == RADIOLIB_ERR_NONE) {
        if (node->setBufferSession(loraSession) != RADIOLIB_ERR_NONE) {
            ESP_LOGW(TAG, "session restore failed — fresh join");
        } else {
            ESP_LOGI(TAG, "session restored");
        }
    }
    return true;
}

bool LoRaWanTransport::sendReceive(const uint8_t *payload, size_t len,
                                   Downlink &dl, uint8_t fport, bool wantTime)
{
    if (!node) {
        return false;
    }

    // Join + RX-window sequence blocks for 10+ s (RX1 delay alone is
    // network-configurable to 5 s) — longer than the 5 s task-watchdog
    // window. Pause our subscription for the duration; the app re-feeds
    // as soon as we return. No-op if the caller never subscribed.
    const bool wdtActive = (esp_task_wdt_status(nullptr) == ESP_OK);
    auto wdtResume = [&]() {
        if (wdtActive) {
            esp_task_wdt_add(nullptr);
        }
    };
    if (wdtActive) {
        esp_task_wdt_delete(nullptr);
    }

    int16_t state = RADIOLIB_ERR_WRONG_MODEM;
    for (int attempt = 0; attempt < 2; ++attempt) {
        state = node->activateOTAA();
        // NEW_SESSION / SESSION_RESTORED: joined now or session restored
        // from RTC. ERR_NONE: already activated — normal for the second
        // sendReceive call per cycle (diagnostics follows telemetry);
        // rejecting it used to skip the diag uplink entirely.
        if (state == RADIOLIB_LORAWAN_NEW_SESSION ||
            state == RADIOLIB_LORAWAN_SESSION_RESTORED ||
            state == RADIOLIB_ERR_NONE) {
            break;
        }
        ESP_LOGW(TAG, "OTAA attempt %d failed (%d)", attempt + 1, state);
        vTaskDelay(pdMS_TO_TICKS(9'700)); // RX window spacing (v1 parity)
    }
    if (state != RADIOLIB_LORAWAN_NEW_SESSION &&
        state != RADIOLIB_LORAWAN_SESSION_RESTORED &&
        state != RADIOLIB_ERR_NONE) {
        wdtResume();
        return false;
    }

    // Network time (feature #2): queue DeviceTimeReq in the FOpts; the
    // network answers in the downlink slot. 8 bytes of FOpts, negligible.
    if (wantTime) {
        node->sendMacCommandReq(RADIOLIB_LORAWAN_MAC_DEVICE_TIME);
    }

    uint8_t downlink[64] = {};
    size_t downlinkLen = 0;
    state = node->sendReceive(const_cast<uint8_t *>(payload), len, fport,
                              downlink, &downlinkLen);

    // Persist join state for next cycle — MUST happen after the data
    // uplink: getBufferSession() serializes the live frame counters, and
    // saving before the TX stored a stale fCntUp (0 right after a join).
    // The restored session then re-sent the same FCnt and TTN silently
    // dropped every restored-cycle uplink as a replay, so the device
    // only ever reached TTN on cycles that re-joined (bench 2026-11-01).
    memcpy(loraNonces, node->getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    memcpy(loraSession, node->getBufferSession(),
           RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
    loraHasSession = true;
    ESP_LOGI(TAG, "uplink fPort %u state=%d downlink=%u bytes", fport, state,
             (unsigned)downlinkLen);

    wdtResume();

    if (wantTime) {
        uint32_t ts = 0;
        uint16_t ms = 0;
        if (node->getMacDeviceTimeAns(&ts, &ms) == RADIOLIB_ERR_NONE) {
            dl.epochSec = ts; // Unix seconds (returnUnix defaults true)
        }
    }

    if (state >= RADIOLIB_ERR_NONE && downlinkLen >= 1) {
        ESP_LOGI(TAG, "downlink %u bytes", downlinkLen);
        if (downlink[0] == kDlMagic && downlinkLen >= 3) {
            // Remote command (fport 3 ABI) — accepted on any uplink.
            dl.cmd = downlink[1];
            dl.cmdArg = downlink[2];
        }
    }
    return true;
}

void LoRaWanTransport::shutdown()
{
    // Deliberately no delete: LoRaWANNode/SX1262 have non-virtual destructors
    // and the whole heap is discarded at deep sleep anyway. Just park the pins.
    deinitPins();
}

bool LoRaWanTransport::hasSession()
{
    return loraHasSession;
}

void LoRaWanTransport::requestLinkCheck()
{
    if (node) {
        node->sendMacCommandReq(RADIOLIB_LORAWAN_MAC_LINK_CHECK);
    }
}

bool LoRaWanTransport::readLinkCheck(uint8_t *marginDb, uint8_t *gwCnt)
{
    *marginDb = 255;
    *gwCnt = 255;
    if (!node) {
        return false;
    }
    uint8_t margin = 0, count = 0;
    if (node->getMacLinkCheckAns(&margin, &count) != RADIOLIB_ERR_NONE) {
        return false;
    }
    *marginDb = static_cast<uint8_t>(margin > 250 ? 250 : margin);
    *gwCnt = static_cast<uint8_t>(count > 250 ? 250 : count);
    return true;
}

} // namespace minilora
