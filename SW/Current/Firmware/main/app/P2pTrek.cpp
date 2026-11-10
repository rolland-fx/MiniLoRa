// P2P trek mode — two-device bilateral beacon (ICD §10.8).
//
// Both devices wake on the same GPS-anchored window (period = the sleep
// table), exchange their §1 frames in id-ordered sub-slots, and sleep.
// The peer's frame is relayed to the phone over BLE (§10.9) so both maps
// show both tracks. Contact state, adaptive power and the role live in
// RTC RAM; the mesh PSK is the pairing.
//
// Timing contract (§10.8): S0 = lower id TX / higher id RX, S1 = higher
// id TX / lower id RX, S2 = whoever missed their RX re-TXes. The window
// grid anchors to the GPS epoch via the RTC-backed system clock
// (RtcClock.hpp) — esp_timer resets on deep sleep and would desync the
// two devices permanently.
//
// Power: the GPS fix + sensors run in the standard wake, then the
// remaining pre-window gap is spent in LIGHT sleep (~150 µA) instead of
// idling the MCU (~20 mA). Light sleep keeps RAM (trekState, the built
// frame) and the system clock, needs no RTC-RAM handoff and no extra
// boot — the exchange simply continues. Deep sleep would save ~4 µAh
// per wake and cost a reboot + frame stash for nothing.

// Deep sleep would save ~4 µAh
// per wake and cost a reboot + frame stash for nothing.

#include "P2pTrek.hpp"

#include "Sleep.hpp"
#include "../domain/P2pFrame.hpp"
#include "../domain/P2pSchedule.hpp"
#include "../domain/RtcClock.hpp"
#include "../domain/Telemetry.hpp"
#include "../drivers/Pins.hpp"
#include "../transports/ble/BleTransport.hpp"
#include "../transports/p2p/P2pRadio.hpp"

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

namespace minilora {

// App.cpp — the RTC-backed clock's validity flag (set from GPS, kept by
// the RTC timer across deep sleep; a power loss clears it).
extern bool g_rtcTimeValid;

namespace p2ptrek {

namespace {
const char *TAG = "trek";

constexpr uint32_t kStateMagic = 0x5452454C; // "TREL" (v2: roles random)
constexpr uint8_t kMaxMiss = 3;              // windows before LED signal
constexpr uint32_t kSlotWakeMarginMs = 300;  // radio re-init before a slot
constexpr uint32_t kMinGapMs = 30;           // below this, don't bother sleeping
constexpr uint32_t kPreWindowMs = 120'000;   // GPS budget before the window

// The power ladder (ICD §10.8): 14 dBm default, +22 legal on 869.525.
const uint8_t kPowerLadder[4] = {14, 17, 20, 22};

struct State {
    uint32_t magic;
    uint8_t powerIdx;    // index into kPowerLadder
    uint8_t missCount;   // consecutive windows without a valid peer frame
    uint8_t goodStreak;  // consecutive strong-RX frames (step-down counter)
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

/// Long idle: light-sleep until `targetMs` (GPS epoch). RAM, the RTC
/// state and the system clock all survive; the 5 s task WDT cannot see
/// a 100 s pause, so the task is unsubscribed for the duration.
/// No-op when the wait is trivial.
void sleepUntilMs(uint64_t targetMs)
{
    const uint64_t nowMs = rtcSystemMs();
    if (targetMs <= nowMs + kMinGapMs) {
        return;
    }
    esp_sleep_enable_timer_wakeup((targetMs - nowMs) * 1000ULL);
    // Pending UART output aborts light sleep — let the log drain first.
    vTaskDelay(pdMS_TO_TICKS(2));
    esp_task_wdt_delete(nullptr);
    esp_light_sleep_start();
    // The wake reason doesn't matter: an early wake (spurious GPIO IRQ)
    // just falls into the crystal-accurate fine alignment below.
    esp_task_wdt_add(nullptr);
    esp_task_wdt_reset();
}

/// Busy-wait (task-sleep) until `targetMs` — crystal-accurate final
/// alignment after the coarse light sleep. A long remainder (spurious
/// early wake) light-sleeps first: a > 5 s vTaskDelay would starve the
/// task WDT.
void alignToMs(uint64_t targetMs)
{
    if (targetMs > rtcSystemMs() + 1000) {
        sleepUntilMs(targetMs - 800);
    }
    for (;;) {
        const int64_t remainMs =
            static_cast<int64_t>(targetMs) - static_cast<int64_t>(rtcSystemMs());
        if (remainMs <= 2) {
            return;
        }
        feedWatchdog();
        vTaskDelay(pdMS_TO_TICKS(remainMs));
    }
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
    const uint32_t slotLen = p2p::airParams(profile).slotLenMs;

    // ---- sleep tail (shared by every exit path) ----
    // prepareSleep() never returns: it arms the timer + EXT1 wake and
    // enters deep sleep. nextWindowMs = 0 falls back to one plain period.
    auto armSleep = [&](uint64_t nextWindowMs) {
        const uint64_t nowMs = rtcSystemMs();
        uint64_t sleepMs;
        if (nextWindowMs == 0 || ctx.batCritical) {
            sleepMs = periodMs;
        } else {
            const uint64_t wakeAt =
                nextWindowMs > kPreWindowMs ? nextWindowMs - kPreWindowMs : nowMs + 1000;
            sleepMs = wakeAt > nowMs ? wakeAt - nowMs : 1000;
        }
        ESP_LOGI(TAG, "sleeping %lu ms", (unsigned long)sleepMs);
        prepareSleep(static_cast<uint32_t>(sleepMs));
        *slept = true;
    };
    auto nextWindowAfter = [&](uint64_t nowMs) {
        return nextWindowStartMs(periodMs, nowMs + 1000);
    };

    // The window grid is GPS-anchored. Without a valid clock there is no
    // grid — an honest miss (the peer counts one too), plain period sleep.
    if (!g_rtcTimeValid) {
        ESP_LOGW(TAG, "no RTC time — unanchored grid, skipping this window");
        ++trekState.missCount;
        prepareSleep(periodMs);
        *slept = true;
        return;
    }

    const uint64_t nowMs = rtcSystemMs();
    const uint64_t windowStart = nextWindowStartMs(periodMs, nowMs);

    // Role: id-order once the peer is known; a FRESH random assumption per
    // window during bootstrap. A deterministic assumption shared by both
    // devices (the old flip) collides forever — random gives ~75% contact
    // per window counting the S2 retry.
    const uint32_t myId = originId();
    const bool iAmLower = trekState.peerKnown
                              ? (myId < trekState.peerId)
                              : (esp_random() & 1);

    const uint64_t s0 = windowStart;
    const uint64_t s1 = windowStart + slotLen;
    const uint64_t s2 = windowStart + 2 * static_cast<uint64_t>(slotLen);
    const uint64_t myTxMs = iAmLower ? s0 : s1;

    // Late guard: if the boot + GPS fix ate the 120 s budget and the
    // window is already underway, a late TX would spill across slots and
    // collide with the peer's S2 retry — skip the window honestly.
    if (s0 <= nowMs + kMinGapMs) {
        ESP_LOGW(TAG, "window %lu already underway — skipped (miss %u)",
                 (unsigned long)(windowStart / periodMs), trekState.missCount);
        ++trekState.missCount;
        armSleep(nextWindowAfter(nowMs));
        return;
    }

    // ---- pre-window gap: light sleep (the GPS fix + sensors already ran;
    // the radio is still parked, the MCU drops to ~150 µA) ----
    sleepUntilMs(s0 - kSlotWakeMarginMs);

    P2pRadio radio;
    if (!radio.begin(profile, txPowerDbm())) {
        armSleep(nextWindowAfter(rtcSystemMs()));
        return;
    }
    feedWatchdog();

    // ---- my frame (one build per window; the S2 retry reuses it) ----
    uint8_t frame[p2p::kMaxBody + p2p::kOverhead];
    const size_t fn = p2p::buildFrame(p2p::kTypeTelemetry, myId,
                                      ++trekSeq, p2p::bootCount(),
                                      ctx.ownPayload, ctx.ownLen,
                                      g_cfg->cfgMeshKey, frame);
    if (fn == 0) {
        ESP_LOGW(TAG, "frame build failed — skipped window");
        radio.park();
        armSleep(nextWindowAfter(rtcSystemMs()));
        return;
    }

    // ---- per-window RX bookkeeping ----
    uint8_t buf[p2p::kMaxBody + p2p::kOverhead];
    uint8_t ptype;
    uint32_t pOrigin = 0, pSeq = 0, lastSeq = 0;
    uint8_t pBody[p2p::kMaxBody];
    bool heardPeer = false;
    bool haveSeq = false;
    float rssi = 0;

    // Receive one slot starting exactly at `slotStartMs`; process the
    // frame (peer learning, ladder, relay) unless it is a duplicate of
    // one already handled this window. Returns true when the peer was
    // heard (first receipt or duplicate).
    auto rxSlot = [&](uint64_t slotStartMs) {
        alignToMs(slotStartMs);
        const int got = radio.receive(buf, sizeof(buf), slotLen, &rssi,
                                      nullptr);
        if (got <= 0 ||
            p2p::parseFrame(buf, static_cast<size_t>(got),
                            g_cfg->cfgMeshKey, &ptype, &pOrigin, &pSeq,
                            pBody, sizeof(pBody)) <= 0 ||
            ptype != p2p::kTypeTelemetry) {
            return false;
        }
        if (haveSeq && pSeq == lastSeq) {
            return true; // the peer's S2 retry of a frame we already have
        }
        haveSeq = true;
        lastSeq = pSeq;
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
        return true;
    };

    auto txSlot = [&](uint64_t slotStartMs) {
        alignToMs(slotStartMs);
        if (radio.channelFree()) {
            radio.send(frame, fn);
            ESP_LOGI(TAG, "own frame sent @%u dBm", txPowerDbm());
        } else {
            ESP_LOGW(TAG, "channel busy — TX skipped");
        }
        feedWatchdog();
    };

    // ---- the exchange (§10.8: S0 lower TX / higher RX, S1 reversed) ----
    if (iAmLower) {
        txSlot(s0);                 // S0: my TX
        heardPeer = rxSlot(s1);     // S1: the peer's frame
    } else {
        heardPeer = rxSlot(s0);     // S0: the peer's frame
        txSlot(s1);                 // S1: my TX
    }
    if (!heardPeer) {
        ++trekState.missCount;
        if (trekState.powerIdx <
            sizeof(kPowerLadder) / sizeof(kPowerLadder[0]) - 1) {
            ++trekState.powerIdx;
            ESP_LOGW(TAG, "miss %u — power up to %u dBm", trekState.missCount,
                     txPowerDbm());
        }
    }
    feedWatchdog();

    // ---- S2: whoever missed their RX retries; the other listens ----
    alignToMs(s2);
    if (!heardPeer) {
        if (radio.channelFree()) {
            radio.send(frame, fn); // the retry the peer is waiting for
        }
    } else {
        // Duplicate of the primary receipt is common here (both frames
        // landed) — rxSlot's seq guard swallows it silently.
        rxSlot(s2);
        // A dup must not clear the miss counter set above... it cannot:
        // heardPeer was already true, so the counter was never bumped.
    }
    feedWatchdog();

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
    armSleep(nextWindowAfter(rtcSystemMs()));
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
