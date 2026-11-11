#include "P2pGateway.hpp"

#include "Sleep.hpp"
#include "../domain/P2pFrame.hpp"
#include "../domain/P2pSchedule.hpp"
#include "../domain/RtcClock.hpp"
#include "../transports/p2p/P2pRadio.hpp"
#include "../transports/wifi/WifiTransport.hpp"

#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

namespace minilora {

// App.cpp — the RTC-backed clock's validity flag (GPS-anchored system
// clock; see P2pTrek.cpp / App.cpp for the anchor + power-cycle rules).
extern bool g_rtcTimeValid;


namespace p2pgw {

namespace {
const char *TAG = "p2pgw";

// ---- RTC state (survives deep sleep, lost on power cycle — the leaf
// history drip self-heals anything lost with it) ----
constexpr uint32_t kStateMagic = 0x50325031; // "P2P1"
constexpr uint8_t kMaxLearn = 12;
constexpr uint8_t kMaxDedup = 32;
constexpr uint32_t kLearnMs = 30 * 60'000;   // awake-learn window (bench 1)
constexpr uint32_t kWakeOverheadMs = 400;    // boot+radio before the slot

struct LearnEntry {
    uint32_t origin;
    uint32_t lastSlotSec; // GPS epoch of the last heard frame (phase)
    uint32_t sleepMinSec; // the leaf's own cadence, from its payload
};
struct DedupEntry {
    uint32_t origin;
    uint32_t seq;
};
RTC_DATA_ATTR uint32_t stateMagic = 0;
RTC_DATA_ATTR uint8_t mode = 0; // 0 = awake-learn, 1 = grid
RTC_DATA_ATTR LearnEntry learn[kMaxLearn];
RTC_DATA_ATTR uint8_t learnN = 0;
RTC_DATA_ATTR DedupEntry dedup[kMaxDedup];
RTC_DATA_ATTR uint8_t dedupN = 0;
RTC_DATA_ATTR uint32_t ownSeq = 0;
// WiFi path state for THIS wake: false = never tried, true = connected.
// Forward-starvation fix: the old code set g_wifiOk after the FIRST
// successful push and then skipped every later forward in the same wake
// — with 2+ leaves heard in one catch slot only the first leaf's frame
// reached the server. "Connected" is now latched per wake (the connect
// attempt is the expensive part, pushes after it are cheap); the skip
// only happens on a FAILED attempt or when no WiFi is configured.
RTC_DATA_ATTR bool wifiUp = false;
RTC_DATA_ATTR bool wifiTried = false;

void initState()
{
    if (stateMagic != kStateMagic) {
        stateMagic = kStateMagic;
        mode = 0;
        learnN = 0;
        dedupN = 0;
        ownSeq = 0;
        memset(learn, 0, sizeof(learn));
        memset(dedup, 0, sizeof(dedup));
    }
}

const DeviceConfig *g_cfg = nullptr;
P2pRadio *g_radio = nullptr;

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

bool seenAlready(uint32_t origin, uint32_t seq)
{
    for (int i = 0; i < dedupN; ++i) {
        if (dedup[i].origin == origin && dedup[i].seq == seq) {
            return true;
        }
    }
    return false;
}

void remember(uint32_t origin, uint32_t seq)
{
    if (dedupN < kMaxDedup) {
        dedup[dedupN++] = {origin, seq};
        return;
    }
    // Ring: overwrite the oldest quarter — frames self-heal via the
    // leaf history drip, so a rare re-forward is harmless.
    memmove(dedup, dedup + kMaxDedup / 4,
            sizeof(DedupEntry) * (kMaxDedup - kMaxDedup / 4));
    dedupN = kMaxDedup - kMaxDedup / 4;
    dedup[dedupN++] = {origin, seq};
}

/// Forward one heard body under the ORIGIN's identity: the server keys it
/// by ?device=. The endpoint MUST match the frame type: the server decodes
/// /data as 28-byte telemetry and /diag as a schema-0x03/04/05 diag frame —
/// forwarding a leaf's diag body to /data fabricated a readings row with
/// temp = 4 °C / hum = fwMajor (schema byte 4 read as temperature), and
/// 24/29-byte diag frames broke /data's 28-multiple check entirely.
void forward(const uint8_t *body, size_t bodyLen, uint32_t origin,
             bool isDiag) {
    if (g_cfg->wifiSSID[0] == '\0' || (wifiTried && !wifiUp)) {
        return; // no WiFi configured, or this wake's connect already failed
    }
    if (!wifiUp) {
        wifiTried = true;
        // Probe the path with the first frame; a failure marks the wake
        // (no more connect attempts this cycle — each costs seconds).
        if (!WifiTransport::start(g_cfg->wifiSSID, g_cfg->wifiPassword,
                                  10'000)) {
            ESP_LOGW(TAG, "wifi connect failed — forwards off this wake");
            return;
        }
        wifiUp = true;
    }
    char name[12];
    snprintf(name, sizeof(name), "%08lX", static_cast<unsigned long>(origin));
    if (WifiTransport::postPayload(body, bodyLen, g_cfg->wifiSSID,
                                   g_cfg->wifiPassword, g_cfg->wifiServerIP,
                                   g_cfg->wifiServerPort, name,
                                   10'000,
                                   isDiag ? "/diag" : "/data")) {
        ESP_LOGI(TAG, "forwarded %08lX (%s)",
                 static_cast<unsigned long>(origin), isDiag ? "diag" : "tele");
    }
}

/// Handle one received on-air frame: authenticate, dedup, learn, forward.
/// `nowSec` MUST be GPS-anchored epoch (rtcNowSec) — the learn table
/// persists across deep sleep and the predicted-window math keys on the
/// same epoch as the slot grid (esp_timer seconds reset every boot; the
/// old stamping desynced the phase after every reboot).
void handleAirFrame(const uint8_t *buf, size_t len, uint32_t nowSec)
{
    uint8_t type, body[p2p::kMaxBody];
    uint32_t origin, seq;
    size_t bl = p2p::parseFrame(buf, len, g_cfg->cfgMeshKey, &type, &origin,
                                &seq, body, sizeof(body));
    if (bl == 0 || seenAlready(origin, seq)) {
        return;
    }
    remember(origin, seq);

    // Learn: the §1 body carries sleep_min at its fixed offset (byte 26);
    // a telemetry frame teaches the gateway this leaf's cadence + phase.
    if (type == p2p::kTypeTelemetry && bl >= 28) {
        int slot = -1;
        for (int i = 0; i < learnN; ++i) {
            if (learn[i].origin == origin) {
                slot = i;
            }
        }
        if (slot < 0 && learnN < kMaxLearn) {
            slot = learnN++;
        }
        if (slot >= 0) {
            const uint32_t sleepMin = body[26] ? body[26] : 15;
            learn[slot].origin = origin;
            learn[slot].lastSlotSec = nowSec;
            learn[slot].sleepMinSec = static_cast<uint32_t>(sleepMin) * 60;
            ESP_LOGI(TAG, "learned %08lX (sleep %u min)",
                     static_cast<unsigned long>(origin), (unsigned)sleepMin);
        }
    }
    forward(body, bl, origin, type == p2p::kTypeDiag);
    feedWatchdog();
}

/// Push the gateway's OWN telemetry in its hash-assigned data slot.
void sendOwnFrame(const Context &ctx)
{
    uint8_t frame[p2p::kMaxBody + p2p::kOverhead];
    const size_t n = p2p::buildFrame(p2p::kTypeTelemetry, originId(),
                                     ++ownSeq, p2p::bootCount(),
                                     ctx.ownPayload, ctx.ownLen,
                                     g_cfg->cfgMeshKey, frame);
    if (n == 0) {
        return;
    }
    if (!g_radio->channelFree()) {
        return; // skip this superframe; the next one re-hashes
    }
    if (g_radio->send(frame, n)) {
        ESP_LOGI(TAG, "own frame sent (seq %lu)", (unsigned long)ownSeq);
    }
}

} // namespace

void wakePhase(const Context &ctx, bool *slept)
{
    *slept = false;
    g_cfg = &ctx.cfg;
    initState();
    if (!keyPresent()) {
        ESP_LOGW(TAG, "no mesh key — P2P idle (provision via §4 schema 5)");
        return;
    }

    const auto profile = static_cast<p2p::AirProfile>(g_cfg->cfgP2PProfile);
    P2pRadio radio;
    if (!radio.begin(profile,
                     g_cfg->cfgP2PTxPower ? g_cfg->cfgP2PTxPower : 14)) {
        return;
    }
    g_radio = &radio;
    wifiUp = false;
    wifiTried = false;

    // Grid math keys on the GPS-anchored clock (the slot grid is epoch-
    // based; esp_timer resets every boot). Without RTC time there is no
    // grid — fall back to one catch-slot listen per boot (the learn
    // table's phase self-heals on the next GPS fix).
    const uint32_t nowSec = g_rtcTimeValid
                                ? static_cast<uint32_t>(rtcSystemMs() / 1000)
                                : static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    const bool gridAnchored = g_rtcTimeValid;

    if (mode == 0) {
        // ---- awake-learn (bench step 1): continuous RX, immediate
        // forwarding. Leaves teach (origin, sleep_min, phase) here. ----
        ESP_LOGI(TAG, "learn mode: listening %lu ms", (unsigned long)kLearnMs);
        uint8_t buf[255];
        uint64_t deadline = esp_timer_get_time() / 1000 + kLearnMs;
        while (esp_timer_get_time() / 1000 < deadline) {
            int got = radio.receive(buf, sizeof(buf), 3000, nullptr, nullptr);
            if (got > 0) {
                handleAirFrame(buf, static_cast<size_t>(got),
                               g_rtcTimeValid
                                   ? static_cast<uint32_t>(rtcSystemMs() / 1000)
                                   : static_cast<uint32_t>(esp_timer_get_time() / 1000000));
            }
            feedWatchdog();
        }
        sendOwnFrame(ctx);
        mode = 1; // bench step 2: from now on, the grid rules the sleeps
        radio.park();
        ESP_LOGI(TAG, "learn done — switching to grid mode");
        prepareSleep(1000); // brief sleep; next boot starts grid mode
        *slept = true;
        return;
    }

    // ---- grid mode: this wake covers ONE catch slot (the always-on
    // emergency window) plus any predicted leaf window due within it. ----
    const uint64_t nowMs = static_cast<uint64_t>(nowSec) * 1000;
    const uint64_t catchMs = p2p::nextCatchSlotMs(profile, nowMs);
    const uint32_t slotLen = p2p::airParams(profile).slotLenMs;
    if (gridAnchored && catchMs > nowMs + kWakeOverheadMs) {
        // Not our window yet — arm the RTC timer and sleep; the next boot
        // (same function) does the listening.
        radio.park();
        prepareSleep(static_cast<uint32_t>(catchMs - nowMs - kWakeOverheadMs));
        *slept = true;
        return;
    }
    if (!gridAnchored) {
        ESP_LOGW(TAG, "no RTC time — unanchored grid, listening once per boot");
    }

    // In-window: listen the catch slot.
    uint8_t buf[255];
    int got = radio.receive(buf, sizeof(buf), slotLen, nullptr, nullptr);
    if (got > 0) {
        handleAirFrame(buf, static_cast<size_t>(got),
                       g_rtcTimeValid
                           ? static_cast<uint32_t>(rtcSystemMs() / 1000)
                           : static_cast<uint32_t>(esp_timer_get_time() / 1000000));
    }

    // Predicted leaf windows due within the next ~55 s of this wake.
    // GPS-anchored wall clock (the learn table's phase is epoch-based);
    // the wait is BOUNDED per leaf (55 s) — a stale phase (leaf changed
    // its period, or the table predates a reboot) must cost one catch
    // slot at most, never the whole wake.
    const uint64_t nowMs2 = minilora::rtcSystemMs();
    for (int i = 0; i < learnN; ++i) {
        const uint64_t dueMs =
            (static_cast<uint64_t>(learn[i].lastSlotSec) +
             learn[i].sleepMinSec) * 1000;
        if (dueMs <= nowMs2 || dueMs >= nowMs2 + 55'000) {
            continue;
        }
        const uint64_t slotMs =
            p2p::nextDataSlotMs(learn[i].origin, profile, dueMs);
        if (slotMs > nowMs2 + 55'000) {
            continue; // stale/mismatched phase — not this wake
        }
        if (slotMs > nowMs2 + 100) {
            vTaskDelay(pdMS_TO_TICKS(slotMs - nowMs2));
        }
        got = radio.receive(buf, sizeof(buf), slotLen, nullptr, nullptr);
        if (got > 0) {
            handleAirFrame(buf, static_cast<size_t>(got),
                           g_rtcTimeValid
                               ? static_cast<uint32_t>(rtcSystemMs() / 1000)
                               : static_cast<uint32_t>(esp_timer_get_time() / 1000000));
        }
        feedWatchdog();
    }

    // Own telemetry slot.
    sendOwnFrame(ctx);

    // Next wake: the next catch slot (the learned-window refinement rides
    // the RTC table on the next boot). Same clock discipline as above.
    const uint64_t nowMs3 =
        gridAnchored ? minilora::rtcSystemMs()
                     : static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const uint64_t nextCatch = p2p::nextCatchSlotMs(profile, nowMs3 + 1000);
    const uint64_t sleepMs =
        nextCatch > nowMs3 + kWakeOverheadMs
            ? nextCatch - nowMs3 - kWakeOverheadMs
            : 1000;
    radio.park();
    ESP_LOGI(TAG, "grid wake done — sleeping %lu ms", (unsigned long)sleepMs);
    prepareSleep(static_cast<uint32_t>(sleepMs));
    *slept = true;
}

} // namespace p2pgw
} // namespace minilora
