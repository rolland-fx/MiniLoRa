#include "P2pTrek.hpp"

#include "Sleep.hpp"
#include "../domain/P2pFrame.hpp"
#include "../domain/P2pSchedule.hpp"
#include "../domain/Telemetry.hpp"
#include "../drivers/Pins.hpp"
#include "../transports/ble/BleTransport.hpp"
#include "../transports/p2p/P2pRadio.hpp"

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

namespace minilora {

namespace p2ptrek {

namespace {
const char *TAG = "trek";

constexpr uint32_t kStateMagic = 0x5452454B; // "TREK"
constexpr uint8_t kMaxMiss = 3;              // windows before LED signal
constexpr uint32_t kWakeOverheadMs = 400;    // boot+radio before the window

// The power ladder (ICD §10.8): 14 dBm default, +22 legal on 869.525.
const uint8_t kPowerLadder[4] = {14, 17, 20, 22};

struct State {
    uint32_t magic;
    uint8_t powerIdx;    // index into kPowerLadder
    uint8_t missCount;   // consecutive windows without a valid peer frame
    uint8_t goodStreak;  // consecutive strong-RX frames (step-down counter)
    uint8_t assumeLower; // bootstrap role assumption until the peer is known
    uint8_t peerKnown;
    uint32_t peerId;
};
RTC_DATA_ATTR State trekState = {};
RTC_DATA_ATTR uint32_t trekSeq = 0; // per-origin frame counter (§10.3);
                                    // a power cycle resets it — the boot
                                    // counter keeps the keystream unique

void initState()
{
    if (trekState.magic != kStateMagic) {
        trekState.magic = kStateMagic;
        trekState.powerIdx = 0;
        trekState.missCount = 0;
        trekState.goodStreak = 0;
        trekState.assumeLower = 1;
        trekState.peerKnown = 0;
        trekState.peerId = 0;
    }
}

const DeviceConfig *g_cfg = nullptr;

bool keyPresent()
{
    static const uint8_t kZero[16] = {0};
    return memcmp(g_cfg->cfgMeshKey, kZero, 16) != 0;
}

uint32_t originId()
{
    return g_cfg->cfgP2PShortId
               ? g_cfg->cfgP2PShortId
               : static_cast<uint32_t>(g_cfg->devEUI & 0xFFFFFFFFu);
}

uint8_t txPowerDbm()
{
    return kPowerLadder[trekState.powerIdx];
}

} // namespace

void wakePhase(const Context &ctx, bool *slept)
{
    *slept = false;
    g_cfg = &ctx.cfg;
    initState();
    if (!keyPresent()) {
        ESP_LOGW(TAG, "no mesh key — trek idle (provision via §4 schema 5)");
        return;
    }

    const auto profile = static_cast<p2p::AirProfile>(g_cfg->cfgP2PProfile);
    const uint32_t periodMs =
        kSleepTableMs[g_cfg->cfgRange < kSleepTableLen
                          ? g_cfg->cfgRange
                          : kSleepTableLen - 1];
    P2pRadio radio;
    if (!radio.begin(profile, txPowerDbm())) {
        return;
    }

    const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const uint64_t windowStart = nextWindowStartMs(periodMs, nowMs);
    const uint32_t slotLen = p2p::airParams(profile).slotLenMs;

    // Role: id-order once the peer is known; a flipped assumption during
    // bootstrap (expected ≤ 2 windows to first contact).
    const uint32_t myId = originId();
    bool iAmLower = trekState.peerKnown
                        ? (myId < trekState.peerId)
                        : (trekState.assumeLower != 0);
    const uint64_t myTxMs = windowStart + (iAmLower ? 0 : slotLen);
    const uint64_t peerTxMs = windowStart + (iAmLower ? slotLen : 0);

    // Wait out the pre-window time (the wake was slot-aligned; the GPS
    // window and sensors already ran).
    if (myTxMs > nowMs + 5) {
        vTaskDelay(pdMS_TO_TICKS(myTxMs - nowMs));
    }
    feedWatchdog();

    // --- my sub-slot: CAD + own frame ---
    uint8_t frame[p2p::kMaxBody + p2p::kOverhead];
    const size_t fn = p2p::buildFrame(p2p::kTypeTelemetry, myId,
                                      ++trekSeq, p2p::bootCount(),
                                      ctx.ownPayload, ctx.ownLen,
                                      g_cfg->cfgMeshKey, frame);
    if (fn == 0) {
        radio.park();
        return;
    }
    if (radio.channelFree()) {
        radio.send(frame, fn);
        ESP_LOGI(TAG, "own frame sent @%u dBm", txPowerDbm());
    }
    feedWatchdog();

    // --- the peer's sub-slot: listen ---
    uint8_t buf[p2p::kMaxBody + p2p::kOverhead];
    uint8_t ptype;
    uint32_t pOrigin, pSeq;
    uint8_t pBody[p2p::kMaxBody];
    bool heardPeer = false;
    float rssi = 0;
    const int got = radio.receive(buf, sizeof(buf), slotLen, &rssi, nullptr);
    if (got > 0 && p2p::parseFrame(buf, static_cast<size_t>(got),
                                   g_cfg->cfgMeshKey, &ptype, &pOrigin,
                                   &pSeq, pBody, sizeof(pBody)) > 0 &&
        ptype == p2p::kTypeTelemetry) {
        heardPeer = true;
        if (!trekState.peerKnown) {
            trekState.peerKnown = 1;
            trekState.peerId = pOrigin;
            ESP_LOGI(TAG, "first contact — peer %08lX",
                     static_cast<unsigned long>(pOrigin));
        }
        trekState.missCount = 0;
        trekState.goodStreak =
            (rssi > -110.0f) ? static_cast<uint8_t>(trekState.goodStreak + 1)
                             : 0;
        if (trekState.goodStreak >= 3 && trekState.powerIdx > 0) {
            --trekState.powerIdx;
            trekState.goodStreak = 0;
            ESP_LOGI(TAG, "link strong — power down to %u dBm",
                     txPowerDbm());
        } else if (rssi < -115.0f &&
                   trekState.powerIdx <
                       sizeof(kPowerLadder) / sizeof(kPowerLadder[0]) - 1) {
            ++trekState.powerIdx;
            ESP_LOGI(TAG, "link weak (%.0f dBm RSSI) — power up to %u dBm",
                     rssi, txPowerDbm());
        }
        // Relay the peer's position to the phone (ICD §10.9).
        BleTransport::beaconPeer(pBody, 28, pOrigin, pSeq,
                                 g_cfg->globalName, 2500);
    } else {
        ++trekState.missCount;
        if (trekState.powerIdx <
            sizeof(kPowerLadder) / sizeof(kPowerLadder[0]) - 1) {
            ++trekState.powerIdx;
            ESP_LOGW(TAG, "miss %u — power up to %u dBm", trekState.missCount,
                     txPowerDbm());
        }
        if (!trekState.peerKnown) {
            trekState.assumeLower = trekState.assumeLower ? 0 : 1;
        }
    }
    feedWatchdog();

    // --- S2: whoever missed their RX retries; the other listens ---
    const uint64_t s2Start = windowStart + 2 * static_cast<uint64_t>(slotLen);
    const uint64_t s2Now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (s2Start > s2Now) {
        vTaskDelay(pdMS_TO_TICKS(s2Start - s2Now));
    }
    if (!heardPeer) {
        if (radio.channelFree()) {
            radio.send(frame, fn); // the retry the peer is waiting for
        }
    } else {
        const int got2 = radio.receive(buf, sizeof(buf), slotLen, &rssi,
                                       nullptr);
        if (got2 > 0 && p2p::parseFrame(buf, static_cast<size_t>(got2),
                                        g_cfg->cfgMeshKey, &ptype, &pOrigin,
                                        &pSeq, pBody, sizeof(pBody)) > 0 &&
            ptype == p2p::kTypeTelemetry) {
            heardPeer = true; // the peer's retry landed — same handling
            trekState.missCount = 0;
            BleTransport::beaconPeer(pBody, 28, pOrigin, pSeq,
                                     g_cfg->globalName, 2500);
        }
    }
    if (trekState.missCount >= kMaxMiss) {
        // ICD §10.8 contact-lost signal: 3× double-blink, green LED.
        for (int r = 0; r < 3; ++r) {
            for (int b = 0; b < 2; ++b) {
                gpio_set_level(pins::kLedGreen, 1);
                vTaskDelay(pdMS_TO_TICKS(150));
                gpio_set_level(pins::kLedGreen, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            feedWatchdog();
        }
    }
    radio.park();

    // ---- next window ----
    const uint64_t nowMs2 = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const uint64_t nextWindow = nextWindowStartMs(periodMs, nowMs2 + 1000);
    const uint64_t wakeAt = nextWindow - 120'000; // GPS budget + sensors
    uint64_t sleepMs = wakeAt > nowMs2 ? wakeAt - nowMs2 : 1000;
    if (ctx.batCritical) {
        sleepMs = periodMs; // critical battery: skip the early wake
    }
    ESP_LOGI(TAG, "window done (miss %u, power %u dBm) — sleeping %lu ms",
             trekState.missCount, txPowerDbm(), (unsigned long)sleepMs);
    prepareSleep(static_cast<uint32_t>(sleepMs));
    *slept = true;
}

uint64_t nextWindowStartMs(uint32_t periodMs, uint64_t notBeforeMs)
{
    const uint64_t period = periodMs;
    uint64_t start = (notBeforeMs / period) * period;
    if (start < notBeforeMs) {
        start += period;
    }
    return start;
}

} // namespace p2ptrek
} // namespace minilora
