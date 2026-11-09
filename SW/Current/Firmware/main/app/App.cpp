#include "App.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "../domain/DiagPayload.hpp"
#include "../domain/Telemetry.hpp"
#include "../domain/Payload.hpp"
#include "../drivers/I2CBus.hpp"
#include "../drivers/BoardPower.hpp"
#include "../drivers/Bme280.hpp"
#include "../drivers/Lis3dh.hpp"
#include "../drivers/Opt3001.hpp"
#include "../drivers/Pa1010d.hpp"
#include "../drivers/Pins.hpp"
#include "History.hpp"
#include "Ota.hpp"
#include "Sleep.hpp"
#include "Stores.hpp"
#include "Buffer.hpp"
#include "../transports/ble/BleTransport.hpp"
#include "../transports/lorawan/LoRaWanTransport.hpp"
#include "../transports/wifi/WifiTransport.hpp"
#include "../transports/p2p/P2pRadio.hpp"
#include "../domain/P2pFrame.hpp"
#include "../domain/P2pSchedule.hpp"
#include "../transports/zigbee/ZigbeeTransport.hpp"
#include "P2pGateway.hpp"

namespace minilora {

namespace {
const char *TAG = "app";

// ---- v1 timing constants ----
constexpr uint8_t kGpsMinSats = 8; // 2.6.13: raised from v1's 7 — with the
                                   // date/epoch regressions fixed, a fix with
                                   // only 7 sats is more likely multipath
                                   // than useful; the count is still reported
                                   // every cycle (ICD 3.4) either way
constexpr uint16_t kGpsColdIterations = 240; // true cold boot (power-on)
constexpr uint32_t kConfigTimeoutMs = 120'000; // v1 parity: 2 min window
constexpr uint32_t kBeaconDurationMs = 2'500;
constexpr uint32_t kWifiTimeoutMs = 10'000;

RTC_DATA_ATTR int16_t g_prevAcc[3] = {0}; // last cycle accel (motion delta)
// P2P frame counter (ICD §10.3) — per-origin, persists across deep sleep;
// a power cycle resets it, which the server reads as a counter reset (the
// insights module treats resets as new spans, never as loss bursts).
RTC_DATA_ATTR uint32_t g_p2pSeq = 0;

// Remote-command flags (fport 3): set mid-cycle by a downlink, consumed on
// the NEXT cycle (RTC-persistent, so they survive the deep sleep between).
RTC_DATA_ATTR bool g_locateNow = false; // next cycle: forced GPS + 60 s sleep
RTC_DATA_ATTR bool g_sendDiag = false;  // next cycle: diagnostics uplink
RTC_DATA_ATTR bool g_bleWindow = false; // next cycle: BLE config service
RTC_DATA_ATTR bool g_otaRequested = false; // next cycle: pull FW from server
// WiFi restore after an OTA window (2.6.16): an OTA_UPDATE received while
// WiFi is disabled temporarily forces cfgWiFi on so the pull can happen;
// once the OTA flag is consumed, the previous state is restored exactly
// once (see the cycle-start block). RTC flags survive deep sleep; a power
// loss simply drops the restore and leaves the NVS state in charge.
RTC_DATA_ATTR bool g_wifiForOta = false;
RTC_DATA_ATTR uint8_t g_wifiRestoreTo = 0;
// Time (V1 parity): the system clock is ANCHORED — a GPS fix or the
// LoRaWAN DeviceTimeAns calls settimeofday(), and the RTC timer keeps
// that time across deep sleep. No accumulator, no wake-type guards:
// every cycle reads the clock; a power loss clears g_rtcTimeValid and
// the frame ships an honest epoch 0 until the next anchor.
RTC_DATA_ATTR bool g_rtcTimeValid = false;
// Field-debugging state (diag v4 bytes 24-27)
RTC_DATA_ATTR uint8_t g_lastTtff = 255;      // last successful TTFF, s
RTC_DATA_ATTR uint8_t g_linkMargin = 255;    // LinkCheck margin, dB
RTC_DATA_ATTR uint8_t g_gwCnt = 255;         // gateways that heard us
RTC_DATA_ATTR uint8_t g_otaLastStatus = 0;   // kOtaStatus*
// Asset mode (P5): motion-wake bookkeeping
RTC_DATA_ATTR bool g_motionWake = false;     // last wake was LIS3DH INT1
RTC_DATA_ATTR bool g_buttonWake = false;     // last wake was BP1 (GPIO7)
RTC_DATA_ATTR bool g_wifiOkLast = false;     // last cycle's WiFi push result
RTC_DATA_ATTR uint8_t g_burstCount = 0;      // motion-GPS bursts today
RTC_DATA_ATTR uint32_t g_burstDay = 0;       // anchor-day of the count
RTC_DATA_ATTR uint8_t g_histSkip = 0;        // no-fix cycles since last fPort-4

/** Anchor the system clock from an authoritative epoch (GPS fix or
 *  DeviceTimeAns). The RTC timer keeps it across deep sleep; a power
 *  loss wipes it and g_rtcTimeValid together (honest zeros follow). */
static void anchorSystemTime(uint32_t epochSec)
{
    struct timeval tv = { .tv_sec = static_cast<time_t>(epochSec),
                          .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    g_rtcTimeValid = true;
}

/** Current epoch from the RTC-backed system clock. Only meaningful
 *  when g_rtcTimeValid is set. */
static uint32_t rtcNowSec()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint32_t>(tv.tv_sec);
}


DeviceConfig g_cfg{};

void feed() { feedWatchdog(); }

bool flagOn(uint8_t f) { return f != 0; }

// ---- P2P star mode (ICD §10) ------------------------------------------
// cfgLoRaMode selects the air interface; a zeroed mesh key keeps the P2P
// radio silent regardless (no key → no P2P, LoRaWAN flags untouched).
static bool p2pKeyPresent()
{
    static const uint8_t kZero[16] = {0};
    return std::memcmp(g_cfg.cfgMeshKey, kZero, sizeof(kZero)) != 0;
}
static bool p2pLeafMode()
{
    return flagOn(g_cfg.cfgLoRaMode) && !flagOn(g_cfg.cfgP2PRole) &&
           p2pKeyPresent();
}
static bool p2pGatewayMode()
{
    return flagOn(g_cfg.cfgLoRaMode) && flagOn(g_cfg.cfgP2PRole) &&
           p2pKeyPresent();
}
static uint32_t p2pOriginId()
{
    return g_cfg.cfgP2PShortId
               ? g_cfg.cfgP2PShortId
               : static_cast<uint32_t>(g_cfg.devEUI & 0xFFFFFFFFu);
}
static uint64_t rtcNowMs()
{
    return static_cast<uint64_t>(rtcNowSec()) * 1000;
}

bool longPressDetected()
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
        return false;
    }
    uint32_t pressedMs = 0;
    while (gpio_get_level(pins::kButtonWakeup) == 0 && pressedMs < 5'000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        pressedMs += 10;
    }
    return pressedMs >= 3'000;
}

/// Read all enabled sensors into `t`. Failures increment counters but never
/// block the cycle (explicit failure path, Roadmap §3).
// Firmware build date → epoch (parses __DATE__ = "Mmm dd yyyy").
static uint32_t buildDateEpoch()
{
    static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char mon[4] = {__DATE__[0], __DATE__[1], __DATE__[2], '\0'};
    const char *hit = strstr(kMonths, mon);
    const int month = hit ? static_cast<int>(hit - kMonths) / 3 + 1 : 1;
    const int day = atoi(__DATE__ + 4);
    const int year = atoi(__DATE__ + 7);
    return utcToEpoch(year, month, day, 0, 0, 0);
}

// Satellites broadcast "now": a legitimate fix date is within
// [build date − 2 d, build date + 10 y] (self-updating per release —
// no hardcoded year to forget). Anything else is a corrupted NMEA read
// or a hopelessly stale module RTC and must not be shipped.
static bool fixDatePlausible(uint32_t epoch)
{
    const uint32_t build = buildDateEpoch();
    return epoch >= build - 2 * 86400 &&
           epoch <= build + 10ULL * 365 * 86400;
}

void sense(TelemetryData &t, Diagnostics &diag, bool runGps, int gpsBudget)
{
    I2CBus &bus = I2CBus::instance();
    bus.init(pins::kI2cSda, pins::kI2cScl);

    // BME280 first (v1 parity): the sensor must sample before the GPS
    // module and radios warm the board — their heat skews the reading.
    if (flagOn(g_cfg.cfgBME280)) {
        if (auto *dev = bus.addDevice(pins::kAddrTemp)) {
            Bme280 bme(dev);
            if (bme.init()) {
                // 4 samples ~50 ms apart, averaged (v1 parity) — the payload
                // is 1°C/1% steps, but averaging drowns the x8 oversampling
                // noise spikes that a single shot lets through.
                constexpr int kBmeSamples = 4;
                float temp = 0, hum = 0, pres = 0;
                bool any = false;
                int good = 0;
                for (int i = 0; i < kBmeSamples; ++i) {
                    Bme280Reading r{};
                    if (bme.read(r)) {
                        temp += r.temperatureC;
                        hum += r.humidityPct;
                        pres += r.pressurePa;
                        ++good;
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if (good) {
                    t.tempC = static_cast<int8_t>(
                        std::lround(temp / good));
                    t.humPct = static_cast<uint8_t>(
                        std::lround(hum / good));
                    t.presDeciHpa = static_cast<uint16_t>(
                        std::lround(pres / (good * 10))); // 0.1 hPa units:
                        // Pa/10; the old /1000 read ~10x low (103 hPa for a
                        // 1030 hPa day) and /100 gave only 1 hPa steps
                } else {
                    ++diag.sensorReadFailures;
                }
            } else {
                ++diag.sensorReadFailures;
            }
        }
    }

    if (flagOn(g_cfg.cfgGNSS)) {
        gpio_set_level(pins::kGnssWake, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    feed();

    if (flagOn(g_cfg.cfgLIS3DH)) {
        if (auto *dev = bus.addDevice(pins::kAddrAcc)) {
            Lis3dh lis(dev);
            if (lis.init()) {
                // P5: asset mode arms the INT1 motion wake for the coming
                // sleep (active-low open-drain — see configureMotionWake).
                if (flagOn(g_cfg.cfgAssetMode)) {
                    lis.configureMotionWake(g_cfg.cfgMotionSens);
                }
                // P2: 4 samples (was 8) — still a solid average for the
                // motion delta, half the awake time.
                constexpr int kAccSamples = 4;
                int32_t sum[3] = {};
                int good = 0;
                for (int i = 0; i < kAccSamples; ++i) {
                    int16_t a[3];
                    if (lis.read(a)) {
                        for (int k = 0; k < 3; ++k) {
                            sum[k] += a[k];
                        }
                        ++good;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
                if (good) {
                    for (int k = 0; k < 3; ++k) {
                        t.acc[k] = static_cast<int16_t>(sum[k] / good);
                    }
                } else {
                    ++diag.sensorReadFailures;
                }
                if (!flagOn(g_cfg.cfgAssetMode)) {
                    // v1 parity (prepareSleep): park the accelerometer.
                    // ODR=0 keeps the config registers but drops the ~4 µA
                    // a 400 Hz normal mode would draw through every deep
                    // sleep. Asset mode leaves the motion-wake config
                    // armed instead (configureMotionWake above); the next
                    // cycle's init() reconfigures either way.
                    lis.powerDown();
                }
            }
        }
    }
    feed();

    if (flagOn(g_cfg.cfgOPT30)) {
        if (auto *dev = bus.addDevice(pins::kAddrLum)) {
            Opt3001 opt(dev);
            if (opt.init()) {
                // P2: 2 samples (was 4) — payload resolution is 1 lux.
                constexpr int kOptSamples = 2;
                float lux = 0;
                int good = 0;
                for (int i = 0; i < kOptSamples; ++i) {
                    float l;
                    if (opt.read(l)) {
                        lux += l;
                        ++good;
                    }
                }
                if (good) {
                    t.lux = static_cast<uint16_t>(lux / good);
                } else {
                    ++diag.sensorReadFailures;
                }
                opt.shutdown();
            }
        }
    }
    feed();

    // Battery first: the low-power strategy (P4) needs it before the GPS
    // decision, and it is the cheapest high-value read.
    t.batMv = BoardPower::instance().readBatteryMv();

    if (flagOn(g_cfg.cfgGNSS) && runGps) {
        // Wake the GNSS BEFORE probing the bus (v1 parity: GNSS_WK HIGH +
        // settle delay). GNSS_WK is held LOW through deep sleep, and a
        // PA1010D in standby doesn't ACK its I2C address — probing first
        // made every cycle silently skip the GPS (12-second cycles).
        gpio_set_level(pins::kGnssWake, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        feed();
        if (auto *dev = bus.addDevice(pins::kAddrGnss)) {
            Pa1010d gps(dev);
            gps.init(pins::kGnssWake);
            const int64_t t0 = esp_timer_get_time();
            bool fixed = false;
            for (uint16_t i = 0; i < gpsBudget; ++i) {
                feed();
                GpsFix f;
                gps.poll(f);
#if CONFIG_MINILORA_GPS_BENCH
                if (i % 5 == 0) {
                    const auto &st = gps.stats();
                    ESP_LOGI(TAG, "gps bench %3ds: sats=%u fix=%d rmc=%lu "
                             "gga=%lu bad=%lu",
                             i, f.sats, (int)gps.hasFix(),
                             (unsigned long)st.rmc, (unsigned long)st.gga,
                             (unsigned long)st.badChecksum);
                }
#endif
                if (!fixed && f.sats >= kGpsMinSats && f.latDeg != 0.0) {
                    // V1 parity (2.6.10): the position ships AS SOON AS it
                    // is valid. The 2.6.4 date gate made the position
                    // hostage to the GPS date, which trails the fix by
                    // tens of seconds on cold-ish starts (2026-11-03
                    // 21:02: a 161-s fix with 16 sats discarded because
                    // the date hadn't decoded inside the window). The
                    // epoch no longer depends on the GPS date — 2.6.8
                    // anchors the RTC and the payload falls back to it —
                    // and the checksum gate covers the corruption the
                    // date check used to guard.
                    fixed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (fixed) {
                const int32_t ttffSec =
                    static_cast<int32_t>((esp_timer_get_time() - t0) / 1000000);
                ESP_LOGI(TAG, "GPS fixed, TTFF %d s (budget %d)", ttffSec,
                         gpsBudget);
                g_lastTtff = static_cast<uint8_t>(
                    ttffSec > 250 ? 250 : ttffSec);
                const GpsFix &f = gps.lastFix();
                // V1 parity: the position ships on validity alone. The
                // GPS date, when it decoded, is the best epoch there is —
                // and it re-anchors the RTC for the cycles ahead.
                diag.gpsConsecFails = 0; // a position IS a fix
                t.latMicro = static_cast<int32_t>(f.latDeg * 1e6);
                t.lonMicro = static_cast<int32_t>(f.lonDeg * 1e6);
                t.altM = static_cast<int16_t>(f.altM);
                t.sats = f.sats;
                t.gpsValid = true;
                if (f.timeValid) {
                    const uint32_t gpsEpoch =
                        utcToEpoch(f.year, f.month, f.day, f.hour, f.min,
                                   f.sec);
                    if (fixDatePlausible(gpsEpoch)) {
                        t.epochSec = gpsEpoch;
                        anchorSystemTime(gpsEpoch);
                    }
                }
                // Without a plausible GPS date the epoch stays 0: the
                // payload phase substitutes the RTC clock (2.6.8) — an
                // honest stamp for a perfectly good position.
                // History AFTER the assignment — the original call site
                // read the not-yet-set t fields and recorded
                // (0, 0, 0, 0) on every fix, poisoning the fPort-4
                // trail.
                hist::append(t.latMicro, t.lonMicro, t.epochSec,
                             t.sats);
            } else {
                // Explicit no-fix path: zeros + counters (never stale garbage)
                ++diag.gpsTimeouts;
                if (diag.gpsConsecFails < UINT8_MAX) {
                    ++diag.gpsConsecFails;
                }
                // ICD 3.4: report what the module tracked during the window
                // (0 if it never emitted a GGA). The fix signal lives in the
                // position bytes — sats is now "last seen tracked", always.
                t.sats = gps.lastFix().sats;
                ESP_LOGW(TAG, "GPS timeout after %d iterations", gpsBudget);
            }
            gps.deepStandby();
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(pins::kGnssWake, 0);
        } else {
            // Loud, not silent: a missing ACK must never masquerade as a
            // healthy short cycle.
            ESP_LOGW(TAG, "GNSS not responding on I2C (0x%02X) — skipped",
                     pins::kAddrGnss);
        }
    }
}

/** ICD §3 CFG_SET flag ids → the runtime config's enable flags.
 *  LoRaWAN is deliberately not addressable: it is the lifeline. */
static uint8_t *configFlagById(uint8_t id, DeviceConfig &cfg)
{
    switch (id) {
    case 1: return &cfg.cfgWiFi;
    case 2: return &cfg.cfgZigbee;
    case 3: return &cfg.cfgBLE;
    case 4: return &cfg.cfgGNSS;
    case 5: return &cfg.cfgBME280;
    case 6: return &cfg.cfgOPT30;
    case 7: return &cfg.cfgLIS3DH;
    default: return nullptr;
    }
}

void applyDownlink(const LoRaWanTransport::Downlink &dl, Diagnostics &diag)
{
    if (dl.cmd != kDlCmdNone) {
        ESP_LOGI(TAG, "downlink cmd 0x%02x arg %u", dl.cmd, dl.cmdArg);
        switch (dl.cmd) {
        case kDlCmdLocateNow:
            g_locateNow = true;
            break;
        case kDlCmdSendDiag:
            g_sendDiag = true;
            break;
        case kDlCmdResetDiag:
            // Clear the in-memory counters; the sleep-phase save persists.
            // bootCount stays (diag cadence + backoff phase depend on it).
            diag.unexpectedResets = 0;
            diag.gpsTimeouts = 0;
            diag.loraJoinFailures = 0;
            diag.loraSendFailures = 0;
            diag.wifiFailures = 0;
            diag.zigbeeFailures = 0;
            diag.sensorReadFailures = 0;
            diag.gpsConsecFails = 0;
            break;
        case kDlCmdBleWindow:
            g_bleWindow = true;
            break;
        case kDlCmdOtaUpdate:
            g_otaRequested = true;
            // 2.6.16: the OTA pull needs the WiFi transport; if the user has
            // it disabled, force it on for the OTA window and remember the
            // previous state (restored at the cycle start after the attempt).
            // Idempotent: a repeated OTA_UPDATE while already forced keeps
            // the ORIGINAL restore state, not the forced-on one.
            if (!flagOn(g_cfg.cfgWiFi)) {
                if (!g_wifiForOta) {
                    g_wifiRestoreTo = g_cfg.cfgWiFi;
                }
                g_wifiForOta = true;
                g_cfg.cfgWiFi = 1;
                ConfigStore::save(g_cfg);
                ESP_LOGI(TAG, "wifi temporarily enabled for the OTA "
                         "(restores to %u afterwards)", g_wifiRestoreTo);
            }
            break;
        case kDlCmdConfigSet: {
            // ICD §3 CFG_SET: arg bit7 = value, bits0-3 = flag id.
            // LoRaWAN itself is deliberately not addressable — it is the
            // lifeline. The state confirm rides the next diag (configFlags).
            const uint8_t id = dl.cmdArg & 0x0F;
            const bool on = (dl.cmdArg & 0x80) != 0;
            uint8_t *flag = configFlagById(id, g_cfg);
            if (flag == nullptr) {
                ESP_LOGW(TAG, "config set: unknown flag id %u", id);
                break;
            }
            *flag = on;
            ConfigStore::save(g_cfg);
            g_sendDiag = true;
            ESP_LOGI(TAG, "config set: flag %u -> %d (saved)", id, on);
            break;
        }
        case kDlCmdSetRange: {
            // ICD §3 SET_RANGE: arg = the cfgRange index into kSleepTableMs
            // (the same scale the config envelope and the app's config tab
            // use). Out-of-range indices are ignored (forward-compatible).
            // Takes effect at the end of THIS cycle; the confirm rides the
            // next telemetry frame's sleep-minutes byte.
            if (dl.cmdArg >= kSleepTableLen) {
                ESP_LOGW(TAG, "set range: index %u out of table (%u)",
                         dl.cmdArg, kSleepTableLen);
                break;
            }
            g_cfg.cfgRange = dl.cmdArg;
            ConfigStore::save(g_cfg);
            ESP_LOGI(TAG, "set range: index %u -> %u min (saved)",
                     dl.cmdArg,
                     (unsigned)(kSleepTableMs[dl.cmdArg] / 60000));
            break;
        }
        default:
            ESP_LOGW(TAG, "unknown command 0x%02x", dl.cmd);
            break;
        }
    }
    // Network time: anchor the RTC with the answer; the payload phase and
    // every later cycle read the clock directly (accuracy: sub-second).
    if (dl.epochSec != 0) {
        anchorSystemTime(dl.epochSec);
        ESP_LOGI(TAG, "network time %lu s", (unsigned long)dl.epochSec);
    }
}

/** Schema 0x05: mirror the runtime config's transport/sensor enables into
 *  the diag frame. MUST run immediately before every encodeDiagnostics: a
 *  CFG_SET is applied mid-cycle (at the uplink's RX window) — a diag built
 *  at sense time would confirm the PRE-change state. */
void stampConfigFlags(Diagnostics &diag)
{
    diag.configFlags =
        (flagOn(g_cfg.cfgWiFi) ? kCfgFlagWifi : 0) |
        (flagOn(g_cfg.cfgZigbee) ? kCfgFlagZigbee : 0) |
        (flagOn(g_cfg.cfgBLE) ? kCfgFlagBle : 0) |
        (flagOn(g_cfg.cfgGNSS) ? kCfgFlagGnss : 0) |
        (flagOn(g_cfg.cfgBME280) ? kCfgFlagBme280 : 0) |
        (flagOn(g_cfg.cfgOPT30) ? kCfgFlagOpt3001 : 0) |
        (flagOn(g_cfg.cfgLIS3DH) ? kCfgFlagLis3dh : 0);
}

// ---- BLE status frame (ICD §5, characteristic 0000CAFF) ------------------
// Built fresh on every read: battery is re-measured, diag comes from the
// cycle state (g_statusDiag points at the runCycle-local struct).
Diagnostics *g_statusDiag = nullptr;

size_t statusProvider(uint8_t *out)
{
    if (!g_statusDiag) {
        return 0;
    }
    unsigned mj = 0, mn = 0, pt = 0;
    sscanf(CONFIG_MINILORA_FW_VERSION, "%u.%u.%u", &mj, &mn, &pt);
    size_t i = 0;
    out[i++] = 0x01; // schema
    out[i++] = static_cast<uint8_t>(mj);
    out[i++] = static_cast<uint8_t>(mn);
    out[i++] = static_cast<uint8_t>(pt);
    const auto put16 = [&](uint16_t v) {
        out[i++] = static_cast<uint8_t>(v & 0xFF);
        out[i++] = static_cast<uint8_t>(v >> 8);
    };
    put16(BoardPower::instance().readBatteryMv());
    put16(g_statusDiag->bootCount);
    put16(g_statusDiag->unexpectedResets);
    out[i++] = g_statusDiag->lastResetReason;
    out[i++] = g_statusDiag->gpsConsecFails;
    out[i++] = g_statusDiag->powerFlags;
    out[i++] = static_cast<uint8_t>((g_statusDiag->gpsValid ? kDiagFlagGpsValid
                                                            : 0) |
                                    g_statusDiag->imageFlags);
    out[i++] = LoRaWanTransport::hasSession() ? 1 : 0;
    out[i++] = 0; // reserved
    return i;     // 16 = kStatusSize
}

} // namespace

void runCycle(bool recovery)
{
    // ---- phase: boot ----
    esp_task_wdt_add(nullptr);
    BoardPower::instance().init();
    esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
    if (reason == ESP_SLEEP_WAKEUP_EXT1) {
        // EXT1 bitmap: bit4 = LIS3DH motion (asset mode), bit7 = BP1.
        g_motionWake = (esp_sleep_get_ext1_wakeup_status() >> 4) & 1;
        g_buttonWake = (esp_sleep_get_ext1_wakeup_status() >> 7) & 1;
    } else {
        g_motionWake = false;
        g_buttonWake = false;
    }
    ESP_LOGI(TAG, "cycle start (wake=%d motion=%d recovery=%d)", reason,
             g_motionWake, recovery);
    // Snapshot before any consumer clears the flags (asset mode eats
    // g_motionWake mid-cycle): early wakes must not trust the net-time
    // fallback's full-sleep assumption.
    const bool earlyWake = g_buttonWake || g_motionWake;

    ConfigDefaults defaults = {
        .globalName = CONFIG_MINILORA_GLOBAL_NAME,
        .wifiSSID = "",
        .wifiPassword = "",
        .wifiServerIP = "",
        .wifiServerPort = 5000,
        .joinEUI = 0,
        .devEUI = 0,
        .appKey = nullptr,
        .nwkKey = nullptr,
        .cfgLoRaWAN = true, .cfgWiFi = false, .cfgGNSS = true,
        .cfgBME280 = true, .cfgOPT30 = true, .cfgLIS3DH = true,
        .cfgBLE = true, .cfgZigbee = false,
        .cfgRange = 4,
        // P2P star (§10): factory default is LoRaWAN/leaf; a provisioned
        // mesh key via the config envelope switches the radio over.
        .cfgP2P = false, .cfgP2PGateway = false, .cfgP2PProfile = 0,
        .cfgP2PRxAfterTx = true, .cfgP2PShortId = 0, .meshKey = nullptr,
    };
    ConfigStore::initAndLoad(g_cfg, defaults);
    // Bench diagnostic: what config is the cycle actually running? (answers
    // the 60-s-sleep / no-GPS mystery: defaults vs NVS blob vs garbage)
    ESP_LOGI(TAG,
             "cfg: src=%s range=%u gnss=%u lora=%u ble=%u wifi=%u asset=%u",
             g_cfg.cfgRange == 4 && g_cfg.cfgGNSS && !g_cfg.cfgWiFi
                 ? "defaults?"
                 : "loaded",
             g_cfg.cfgRange, g_cfg.cfgGNSS, g_cfg.cfgLoRaWAN, g_cfg.cfgBLE,
             g_cfg.cfgWiFi, g_cfg.cfgAssetMode);

    Diagnostics diag{};
    diag.fwMajor = 2;
    g_statusDiag = &diag; // live status for BLE config-window reads
    const bool recoveryNow =
        DiagStore::onBoot(diag) || recovery; // boot-loop guard

    // OTA rollback: confirm this image after a healthy config load
    // (idempotent; no-op when the image is already valid).
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t imgState = ESP_OTA_IMG_UNDEFINED;
    const bool otaPending =
        esp_ota_get_state_partition(running, &imgState) == ESP_OK &&
        imgState == ESP_OTA_IMG_PENDING_VERIFY;
    if (otaPending) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image confirmed");
    }
    diag.imageFlags = (otaPending ? kDiagFlagOtaPending : 0) |
                      (recoveryNow ? kDiagFlagRecovery : 0);
    feed();

    // ---- phase: config mode (long press) ----
    // DELIBERATELY ungated on cfgBLE: the long press is the user standing
    // in front of the device — BLE config must always work (2.6.11).
    if (longPressDetected()) {
        ESP_LOGI(TAG, "config mode");
        vTaskDelay(pdMS_TO_TICKS(700));
        auto result = BleTransport::runConfigService(
            g_cfg, g_cfg.globalName, kConfigTimeoutMs, statusProvider);
        if (result.written) {
            g_cfg = result.cfg;
            ConfigStore::save(g_cfg);
            ESP_LOGI(TAG, "config updated (v3 envelope)");
        }
        feed();
        prepareSleep(kSleepTableMs[g_cfg.cfgRange]);
    }

    if (recoveryNow) {
        // Minimal cycle: config window + sleep, no radio TX, no sensors.
        // The config window is ungated (the recovery lifeline).
        ESP_LOGW(TAG, "recovery mode: %u unexpected resets",
                 diag.unexpectedResets);
        BleTransport::runConfigService(g_cfg, g_cfg.globalName,
                                       kConfigTimeoutMs, statusProvider);
        prepareSleep(kSleepTableMs[g_cfg.cfgRange]);
    }

    // ---- phase: remote BLE config window (downlink cmd 0x04) ----
    // Consumed at cycle start so the user has the full window to connect.
    // Ungated on cfgBLE: an explicit remote request implies intent (2.6.11).
    if (g_bleWindow) {
        g_bleWindow = false;
        ESP_LOGI(TAG, "remote command: BLE config window");
        auto result = BleTransport::runConfigService(
            g_cfg, g_cfg.globalName, kConfigTimeoutMs, statusProvider);
        if (result.written) {
            g_cfg = result.cfg;
            ConfigStore::save(g_cfg);
        }
        prepareSleep(kSleepTableMs[g_cfg.cfgRange]);
    }

    // ---- phase: power strategy (P1/P4) ----
    // Battery is read first: every decision below depends on it.
    TelemetryData t{};
    t.sleepIdx = g_cfg.cfgRange;
    t.batMv = BoardPower::instance().readBatteryMv();

    const bool batLow = t.batMv < CONFIG_MINILORA_BAT_LOW_MV;
    const bool batCritical = t.batMv < CONFIG_MINILORA_BAT_CRITICAL_MV;
    diag.powerFlags = (batLow ? kPowerFlagDegraded : 0) |
                      (batCritical ? kPowerFlagCritical : 0);
    // Bench diagnostic: banner battery voltage + reset reason name. This
    // prints ~0.6 s after boot, early enough to survive even a 1-second
    // USB lifetime during reset storms (PWRGLITCH/USB-reset debugging).
    {
        const esp_reset_reason_t r = static_cast<esp_reset_reason_t>(
            diag.lastResetReason);
        const char *rn = "UNKNOWN";
        switch (r) {
        case ESP_RST_POWERON: rn = "POWERON"; break;
        case ESP_RST_EXT: rn = "EXT"; break;
        case ESP_RST_SW: rn = "SW"; break;
        case ESP_RST_PANIC: rn = "PANIC"; break;
        case ESP_RST_INT_WDT: rn = "INT_WDT"; break;
        case ESP_RST_TASK_WDT: rn = "TASK_WDT"; break;
        case ESP_RST_WDT: rn = "WDT"; break;
        case ESP_RST_DEEPSLEEP: rn = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT: rn = "BROWNOUT"; break;
        case ESP_RST_USB: rn = "USB"; break;
        case ESP_RST_JTAG: rn = "JTAG"; break;
        case ESP_RST_PWR_GLITCH: rn = "PWR_GLITCH"; break;
        case ESP_RST_CPU_LOCKUP: rn = "CPU_LOCKUP"; break;
        default: break;
        }
        ESP_LOGI(TAG, "bat %u mV (low=%d critical=%d) reset=%s",
                 t.batMv, batLow, batCritical, rn);
    }

    // ---- phase: user interaction (button wake / BP2 command) ----
    // Only on user-present boots: BP1 wake or power-on. Timer wakes stay
    // silent — a 2-s burst per cycle would eat ~5% of the energy budget.
    if (g_buttonWake || diag.lastResetReason == ESP_RST_POWERON) {
        // Green: battery quartile (CRITICAL_MV..4200 mV → 1-4 blinks)
        int q = (static_cast<int>(t.batMv) - CONFIG_MINILORA_BAT_CRITICAL_MV) *
                    4 / (4200 - CONFIG_MINILORA_BAT_CRITICAL_MV) + 1;
        if (q < 1) q = 1;
        if (q > 4) q = 4;
        for (int i = 0; i < q; ++i) {
            BoardPower::instance().ledGreen(true);
            vTaskDelay(pdMS_TO_TICKS(120));
            BoardPower::instance().ledGreen(false);
            vTaskDelay(pdMS_TO_TICKS(230));
        }
        vTaskDelay(pdMS_TO_TICKS(400));
        // Blue: network state — 1 = not provisioned (devEUI 0),
        // 2 = provisioned without session, 3 = session active
        uint8_t net = 1;
        if (g_cfg.devEUI != 0) {
            net = LoRaWanTransport::hasSession() ? 3 : 2;
        }
        for (uint8_t i = 0; i < net; ++i) {
            BoardPower::instance().ledBlue(true);
            vTaskDelay(pdMS_TO_TICKS(120));
            BoardPower::instance().ledBlue(false);
            vTaskDelay(pdMS_TO_TICKS(230));
        }
        ESP_LOGI(TAG, "status report: bat=%u mV (q%d) net=%u", t.batMv, q,
                 net);
    }
    if (g_buttonWake) {
        // 10-s command window: BP1 woke the device, so the user is standing
        // there. BP2 = locate now (forced GPS + uplink this cycle). GPIO14
        // cannot wake the C6 from deep sleep, so BP2 only works here.
        bool confirmed = false;
        const int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 10'000'000LL) {
            feed();
            if (gpio_get_level(pins::kButton2) == 0) {
                if (!confirmed) {
                    g_locateNow = true;
                    confirmed = true;
                    ESP_LOGI(TAG, "BP2: locate now requested");
                    for (uint8_t i = 0; i < 2; ++i) { // green double-blink
                        BoardPower::instance().ledGreen(true);
                        vTaskDelay(pdMS_TO_TICKS(120));
                        BoardPower::instance().ledGreen(false);
                        vTaskDelay(pdMS_TO_TICKS(160));
                    }
                }
            } else {
                confirmed = false; // released: allow re-trigger
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // GPS budget: cold boot gets the full window; wakes from backup mode are
    // hot/warm starts (ephemeris retained) -> short budget. Indoor backoff
    // (P1): after consecutive failures, GPS runs only every 2^fails cycles
    // (off-cycles skip the GNSS entirely — it stays in 15 uA backup).
    bool runGps = flagOn(g_cfg.cfgGNSS) && !batCritical;
    int gpsBudget = CONFIG_MINILORA_GPS_WARM_ATTEMPTS;
    if (reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        gpsBudget = kGpsColdIterations;
    }
    if (runGps) {
        const int fails =
            diag.gpsConsecFails > 3 ? 3 : diag.gpsConsecFails;
        if (fails > 0 && (diag.bootCount % (1 << fails)) != 0) {
            runGps = false;
            ESP_LOGI(TAG, "GPS skipped (indoor backoff, %u consecutive fails)",
                     diag.gpsConsecFails);
        }
    }
    if (runGps && batLow && (diag.bootCount % 2) != 0) {
        runGps = false; // P4: low battery -> GPS every other cycle
    }
    // Bench visibility: the GPS decision must never be silent again — a
    // skipped window looks identical to a healthy short cycle otherwise.
    ESP_LOGI(TAG, "gps decision: run=%d budget=%d consecFails=%u boot=%u batLow=%d",
             runGps, gpsBudget, diag.gpsConsecFails, diag.bootCount, batLow);

    // Remote LOCATE_NOW (feature #1): force a GPS window this cycle,
    // overriding the indoor backoff and the low-battery skip (but never in
    // critical battery — survival mode stays untouchable).
    bool locateNow = false;
    if (g_locateNow) {
        g_locateNow = false;
        locateNow = true;
        if (flagOn(g_cfg.cfgGNSS) && !batCritical) {
            runGps = true;
            // Full cold budget: a locate request after a long indoor spell
            // starts the module into a cold-ish search (TTFF 30-35 s); the
            // warm budget alone could time out and defeat the command.
            gpsBudget = std::max(gpsBudget,
                                 static_cast<int>(kGpsColdIterations));
            ESP_LOGI(TAG, "locate now: forced GPS window");
        }
    }

#if CONFIG_MINILORA_GPS_BENCH
    // Bench: never suppress the GPS, never use the short budget — with USB
    // power the backoff has no purpose and every boot must test the GNSS.
    runGps = flagOn(g_cfg.cfgGNSS);
    gpsBudget = gpsBudget > 300 ? gpsBudget : 300;
    ESP_LOGW(TAG, "GPS BENCH: window forced, budget %d s", gpsBudget);
#endif

    // ---- phase: OTA (server-pull; reboots on success) ----
    // WiFi restore (2.6.16): once the OTA flag is consumed (attempt done —
    // a success reboots into the new image, a failure just ends the cycle),
    // the pre-OTA WiFi state is restored exactly once. Runs BEFORE the gate
    // and before applyDownlink: a fresh OTA_UPDATE in this cycle must be
    // able to re-arm the window. Never fires while a request is still
    // pending (e.g. skipped for critical battery — the OTA stays armed).
    if (g_wifiForOta && !g_otaRequested) {
        g_wifiForOta = false;
        g_cfg.cfgWiFi = g_wifiRestoreTo;
        ConfigStore::save(g_cfg);
        ESP_LOGI(TAG, "wifi restored to %u after the OTA window",
                 (unsigned)g_wifiRestoreTo);
    }
    // Triggered by the OTA_UPDATE downlink (immediate) or periodically
    // (MINILORA_OTA_CHECK_EVERY boots). Never in critical battery; the
    // explicit command is honored even in degraded mode. On success the
    // device reboots into the new image and skips the rest of the cycle.
    const bool otaDue = CONFIG_MINILORA_OTA_CHECK_EVERY > 0 &&
                        (diag.bootCount % CONFIG_MINILORA_OTA_CHECK_EVERY) == 0;
    const bool otaWanted = g_otaRequested || (otaDue && !batLow);
    if (otaWanted && !batCritical && flagOn(g_cfg.cfgWiFi) &&
        g_cfg.wifiSSID[0] != '\0' && g_cfg.wifiServerIP != 0) {
        g_otaRequested = false;
        ESP_LOGI(TAG, "OTA check (requested=%d due=%d)", g_otaRequested,
                 otaDue);
        uint8_t otaStatus = kOtaStatusNone;
        if (Ota::apply(g_cfg, &otaStatus)) {
            g_otaLastStatus = otaStatus;
            DiagStore::save(diag); // persist counters before the reboot
            esp_restart();
        }
        g_otaLastStatus = otaStatus;
        feed();
    }

    // ---- asset mode (P5): motion wake => burst cycle ----
    bool assetBurst = false;
    if (flagOn(g_cfg.cfgAssetMode) && g_motionWake) {
        g_motionWake = false;
        const uint8_t cap = g_cfg.cfgBurstMaxPerDay ? g_cfg.cfgBurstMaxPerDay
                                                    : 20;
        const uint32_t day = g_rtcTimeValid ? rtcNowSec() / 86400 : 0;
        if (day != g_burstDay) {
            g_burstDay = day;
            g_burstCount = 0;
        }
        if (g_burstCount < cap) {
            g_burstCount++;
            assetBurst = true;
            if (!batCritical && flagOn(g_cfg.cfgGNSS)) {
                runGps = true; // forced window, warm budget
                gpsBudget = std::max(
                    gpsBudget,
                    static_cast<int>(CONFIG_MINILORA_GPS_WARM_ATTEMPTS));
            }
            ESP_LOGI(TAG, "motion burst %u/%u", g_burstCount, cap);
        } else {
            ESP_LOGW(TAG, "motion ignored (daily cap %u reached)", cap);
        }
    }

    // ---- phase: sense ----
    sense(t, diag, runGps, gpsBudget);
    diag.gpsValid = t.gpsValid ? 1 : 0; // diag flags2 bit0 (ICD §2)
    diag.lastTtff = g_lastTtff;
    diag.linkMargin = g_linkMargin;
    diag.gwCnt = g_gwCnt;
    diag.otaStatus = g_otaLastStatus;
    // configFlags are stamped at encode time (stampConfigFlags), not here:
    // a CFG_SET can land mid-cycle and must be reflected by the very next
    // diag frame.
    feed();

    // ---- phase: build payload ----
    // Time (V1 parity): the system clock is anchored by a GPS fix or the
    // LoRaWAN DeviceTimeAns and rides the RTC timer across deep sleep —
    // no accumulator, no wake-type guards, no speculative sleep adds.
    // A power loss clears g_rtcTimeValid and the frame ships an honest
    // epoch 0 until the next anchor re-arms it.
    bool wantNetTime = false;
    const uint32_t awakeMs = static_cast<uint32_t>(
        esp_timer_get_time() / 1000);
    if (t.epochSec == 0) {
        if (g_rtcTimeValid) {
            t.epochSec = rtcNowSec();
            ESP_LOGI(TAG, "epoch from RTC time");
        }
        wantNetTime = true; // keep the DeviceTimeReq cadence: re-sync drift
    }
    if (t.gpsValid && t.epochSec != 0) {
        // A GPS fix is the best time reference there is.
        anchorSystemTime(t.epochSec);
    }
    t.awakeSec = static_cast<uint8_t>(std::min<uint32_t>(awakeMs / 1000, 255));
    t.gpioStatus = 0;
    if (gpio_get_level(pins::kLedGreen)) {
        t.gpioStatus |= 0x01;
    }
    if (gpio_get_level(pins::kButton2Legacy)) {
        t.gpioStatus |= 0x02;
    }

    uint8_t payload[kPayloadSize];
    encodePayload(t, payload);
    feed();

    // ---- phase: BLE beacon ----
    // Tracker profile: the BLE beacon follows the RUNTIME cfgBLE flag —
    // that's what the app's "BLE beacon" toggle writes. The old
    // compile-time MINILORA_DEFAULT_BEACON gate silently overrode it:
    // sdkconfig shipped with it off, so no build ever beacons no matter
    // what the user configured (bench 2026-11-01). Skipped in
    // degraded/critical battery states.
    const bool beaconWanted = flagOn(g_cfg.cfgBLE);
    if (beaconWanted && !batLow && !batCritical) {
        BleTransport::beacon(payload, kPayloadSize, g_cfg.globalName,
                             kBeaconDurationMs);
        feed();
    }

    // ---- phase: LoRaWAN (skipped in P2P mode — one air interface) ----
    bool loraSent = false;
    if (!p2pLeafMode() && !p2pGatewayMode() &&
        flagOn(g_cfg.cfgLoRaWAN) && g_cfg.devEUI != 0) {
        LoRaWanTransport lora;
        LoRaWanTransport::Downlink dl;
        if (lora.begin(g_cfg.joinEUI, g_cfg.devEUI, g_cfg.nwkKey, g_cfg.appKey)) {
            // wantNetTime: ask the network for UTC when GPS time is missing.
            if (lora.sendReceive(payload, kPayloadSize, dl, kFportTelemetry,
                                 wantNetTime)) {
                loraSent = true;
                applyDownlink(dl, diag);
            } else {
                ++diag.loraJoinFailures;
            }
            // Diagnostics uplink: every kDiagSendEveryCycles boots (~1/day)
            // or on demand (remote SEND_DIAG command).
            if (g_sendDiag || diag.bootCount % kDiagSendEveryCycles == 0) {
                g_sendDiag = false;
                uint8_t diagPayload[kDiagPayloadSize];
                unsigned mj = 0, mn = 0, pt = 0;
                sscanf(CONFIG_MINILORA_FW_VERSION, "%u.%u.%u", &mj, &mn, &pt);
                diag.fwMajor = mj;
                diag.fwMinor = mn;
                diag.fwPatch = pt;
                stampConfigFlags(diag); // post-apply state, always (2.6.15)
                encodeDiagnostics(diag, diagPayload);
                // LinkCheck (diag v4): the answer describes THIS uplink and
                // is reported in the NEXT diag frame (RTC-persistent).
                lora.requestLinkCheck();
                LoRaWanTransport::Downlink dl2;
                lora.sendReceive(diagPayload, kDiagPayloadSize, dl2,
                                 kFportDiagnostics);
                lora.readLinkCheck(&g_linkMargin, &g_gwCnt);
                // A remote command may ride on the diag frame's RX slot
                // (only cmd/magic frames parse there, never the v1 ABI).
                applyDownlink(dl2, diag);
            }
        } else {
            ++diag.loraJoinFailures;
        }
        lora.shutdown();
        feed();
    }

    // ---- phase: P2P leaf (ICD §10) — TX in the hash-assigned data slot,
    // then the 2 s command window; diag rides the same cadence as LoRaWAN.
    // Off-grid wakeups (drift) TX immediately — the catch slots + the
    // history drip absorb the loss, and each GPS fix re-tightens the slot.
    if (p2pLeafMode()) {
        P2pRadio radio;
        const auto profile = static_cast<p2p::AirProfile>(g_cfg.cfgP2PProfile);
        if (radio.begin(profile)) {
            const uint32_t origin = p2pOriginId();
            uint8_t frame[kPayloadSize + p2p::kOverhead];
            const size_t fn = p2p::buildFrame(
                p2p::kTypeTelemetry, origin, g_p2pSeq++, p2p::bootCount(),
                payload, kPayloadSize, g_cfg.cfgMeshKey, frame);
            if (fn > 0) {
                // Land on the slot: wait (bounded) when the next hash slot
                // is near; TX immediately when the wake drifted past it.
                const uint64_t nowMs = rtcNowMs();
                const uint64_t slotMs = p2p::nextDataSlotMs(
                    origin, profile, nowMs + 100);
                if (slotMs > nowMs && slotMs - nowMs <= 55'000) {
                    vTaskDelay(pdMS_TO_TICKS(slotMs - nowMs));
                }
                if (radio.channelFree() && radio.send(frame, fn)) {
                    loraSent = true;
                }
                feed();
                // Command window: the gateway answers right after the
                // leaf's frame (same 3-byte §3 body inside a P2P wrapper).
                if (flagOn(g_cfg.cfgP2PRxAfterTx) && loraSent) {
                    uint8_t rxBuf[p2p::kMaxBody + p2p::kOverhead];
                    uint8_t rtype;
                    uint32_t rOrigin, rSeq;
                    const int got = radio.receive(
                        rxBuf, sizeof(rxBuf), 2000, nullptr, nullptr);
                    if (got > 0) {
                        uint8_t body[p2p::kMaxBody];
                        const size_t bl = p2p::parseFrame(
                            rxBuf, static_cast<size_t>(got),
                            g_cfg.cfgMeshKey, &rtype, &rOrigin, &rSeq, body,
                            sizeof(body));
                        if (bl == 3 && rtype == p2p::kTypeCommand &&
                            body[0] == kDlMagic) {
                            LoRaWanTransport::Downlink dl;
                            dl.cmd = body[1];
                            dl.cmdArg = body[2];
                            applyDownlink(dl, diag);
                        }
                    }
                    feed();
                }
                // Diagnostics over P2P: same cadence as the LoRaWAN diag.
                if (g_sendDiag || diag.bootCount % kDiagSendEveryCycles == 0) {
                    g_sendDiag = false;
                    uint8_t diagPayload[kDiagPayloadSize];
                    unsigned mj = 0, mn = 0, pt = 0;
                    sscanf(CONFIG_MINILORA_FW_VERSION, "%u.%u.%u", &mj, &mn,
                           &pt);
                    diag.fwMajor = mj;
                    diag.fwMinor = mn;
                    diag.fwPatch = pt;
                    stampConfigFlags(diag);
                    encodeDiagnostics(diag, diagPayload);
                    uint8_t dframe[kDiagPayloadSize + p2p::kOverhead];
                    const size_t dn = p2p::buildFrame(
                        p2p::kTypeDiag, origin, g_p2pSeq++, p2p::bootCount(),
                        diagPayload, kDiagPayloadSize, g_cfg.cfgMeshKey,
                        dframe);
                    if (dn > 0 && radio.channelFree()) {
                        radio.send(dframe, dn);
                    }
                }
            }
        }
        radio.park();
        feed();
    }

    // ---- phase: WiFi ----
    bool wifiSent = false;
    if (flagOn(g_cfg.cfgWiFi) && !batCritical && g_cfg.wifiSSID[0] != '\0') {
        wifiSent = WifiTransport::postPayload(payload, kPayloadSize,
                                              g_cfg.wifiSSID,
                                              g_cfg.wifiPassword,
                                              g_cfg.wifiServerIP,
                                              g_cfg.wifiServerPort,
                                              g_cfg.globalName,
                                              kWifiTimeoutMs);
        if (!wifiSent) {
            ++diag.wifiFailures;
        }
        // Diagnostics also ride WiFi (server: POST /diag, same 24/28-byte
        // frame as the fPort-2 uplink). Same cadence as LoRaWAN's diag
        // (every kDiagSendEveryCycles boots or a remote SEND_DIAG): if the
        // LoRa phase already sent it, g_sendDiag is cleared and this is
        // skipped; when LoRa is down/disabled this is the only diag source.
        // The snapshot is fresher than the LoRa one (includes this cycle's
        // counters), which is intentional. The extra POST rides the
        // already-up radio — negligible cost at ~1/day.
        if (g_sendDiag || diag.bootCount % kDiagSendEveryCycles == 0) {
            uint8_t diagPayload[kDiagPayloadSize];
            unsigned mj = 0, mn = 0, pt = 0;
            sscanf(CONFIG_MINILORA_FW_VERSION, "%u.%u.%u", &mj, &mn, &pt);
            diag.fwMajor = mj;
            diag.fwMinor = mn;
            diag.fwPatch = pt;
            stampConfigFlags(diag); // post-apply state, always (2.6.15)
            encodeDiagnostics(diag, diagPayload);
            // Best effort: telemetry already counted its wifiFailures.
            WifiTransport::postPayload(diagPayload, kDiagPayloadSize,
                                       g_cfg.wifiSSID, g_cfg.wifiPassword,
                                       g_cfg.wifiServerIP, g_cfg.wifiServerPort,
                                       g_cfg.globalName, kWifiTimeoutMs,
                                       "/diag");
        }
        g_wifiOkLast = wifiSent;
        feed();
    }

    // ---- phase: store-and-forward buffer (offline points) ----
    // A cycle whose payload went nowhere (no join / send failure / push
    // failure) is appended to the NVS ring; a cycle where any transport
    // worked drains it — WiFi in one bulk POST, LoRaWAN by the 2-frame
    // drip in its own phase. Critical-battery cycles skip the append
    // (energy discipline: the transports above are skipped there too).
    // Only frames with a valid GPS position are buffered: a no-fix frame
    // carries no recoverable information (its sensors are stale by the
    // time the buffer drains) and would dilute the position trail.
    if (!loraSent && !wifiSent) {
        if (!batCritical && t.gpsValid) {
            buf::append(payload);
            ESP_LOGI(TAG, "no transport: buffered (%d points pending)",
                     buf::count());
        }
    } else if (wifiSent && buf::count() > 0) {
        uint8_t bulk[buf::kMaxFrames * kPayloadSize];
        const size_t nFrames = buf::readOldest(bulk, buf::kMaxFrames);
        if (nFrames > 0 &&
            WifiTransport::postPayload(bulk, nFrames * kPayloadSize,
                                       g_cfg.wifiSSID, g_cfg.wifiPassword,
                                       g_cfg.wifiServerIP,
                                       g_cfg.wifiServerPort, g_cfg.globalName,
                                       kWifiTimeoutMs)) {
            buf::dropOldest(nFrames);
            ESP_LOGI(TAG, "buffer flushed over WiFi (%d points)", nFrames);
        }
    }
    feed();

    // ---- phase: Zigbee (last: its task keeps the 802.15.4 radio busy) ----
    // Skipped in degraded/critical battery states — LoRaWAN is the tracker's
    // lifeline, Zigbee is a convenience.
    if (flagOn(g_cfg.cfgZigbee) && !batLow && !batCritical) {
        if (ZigbeeTransport::startAndJoin(30'000)) {
            if (!ZigbeeTransport::publish(t, g_prevAcc)) {
                ++diag.zigbeeFailures;
            }
        } else {
            ++diag.zigbeeFailures;
        }
        ZigbeeTransport::persist();
        feed();
    }

    // ---- phase: sleep ----
    // Effective interval (P4): degraded = double (clamped to the table max),
    // critical = max. The payload's SleepMinutes byte keeps reporting
    // cfgRange (payload v1 is frozen); the degraded flag rides the
    // diagnostics uplink so consumers can interpret the difference.
    std::memcpy(g_prevAcc, t.acc, sizeof(g_prevAcc));
    DiagStore::save(diag);
    DiagStore::onCleanCycle();
    uint32_t sleepMs = kSleepTableMs[g_cfg.cfgRange];
    if (batLow) {
        sleepMs = sleepMs * 2 > kSleepTableMs[kSleepTableLen - 1]
                      ? kSleepTableMs[kSleepTableLen - 1]
                      : sleepMs * 2;
    }
    if (batCritical) {
        sleepMs = kSleepTableMs[kSleepTableLen - 1];
    }
    // Remote LOCATE_NOW: report quickly again after this locate cycle
    // (superseded by critical battery, which pins the max interval).
    if (locateNow && !batCritical) {
        sleepMs = 60'000;
    }
    // P2P leaf (ICD §10.4): align the wake to the hash-assigned data slot
    // — the RTC alarm fires 120 s early (GPS window + sensor reads), the
    // cycle then waits out the remainder to TX exactly in-slot. Asset
    // mode and critical battery keep their own timing (the drip heals
    // missed slots).
    if (p2pLeafMode() && !batCritical && !locateNow &&
        !flagOn(g_cfg.cfgAssetMode) && g_rtcTimeValid) {
        const uint64_t notBefore =
            rtcNowMs() + static_cast<uint64_t>(sleepMs) * 1000;
        const uint64_t slotMs = p2p::nextDataSlotMs(
            p2pOriginId(), static_cast<p2p::AirProfile>(g_cfg.cfgP2PProfile),
            notBefore);
        const uint64_t wakeMs = slotMs - 120'000;
        const uint64_t aligned = wakeMs > rtcNowMs()
                                     ? wakeMs - rtcNowMs()
                                     : 0;
        if (aligned > 60'000) {
            sleepMs = static_cast<uint32_t>(
                aligned > kSleepTableMs[kSleepTableLen - 1]
                    ? kSleepTableMs[kSleepTableLen - 1]
                    : aligned);
            ESP_LOGI(TAG, "p2p slot-aligned sleep %u ms", sleepMs);
        }
    }
    // P2P gateway: this whole wake belongs to the slot grid — the radio
    // windows run here and the deep-sleep arming happens inside.
    if (p2pGatewayMode()) {
        const p2pgw::Context gctx{g_cfg, payload, kPayloadSize, batCritical};
        bool slept = false;
        p2pgw::wakePhase(gctx, &slept);
        if (slept) {
            return; // sleep armed for the next catch/predicted window
        }
        ESP_LOGW(TAG, "gateway wake fell through — leaf-style sleep");
        return;
    }
    ESP_LOGI(TAG, "sleeping %u ms (bat %u mV, low=%d critical=%d)", sleepMs,
             t.batMv, batLow, batCritical);
    if (flagOn(g_cfg.cfgAssetMode)) {
        // P5: after a motion burst sleep the burst interval; otherwise the
        // asset heartbeat. Timer wakes (heartbeat) reset nothing — the
        // burst cap is day-based via netEpoch.
        uint32_t assetMs;
        if (assetBurst) {
            const uint8_t bmin = g_cfg.cfgBurstIntervalMin
                                     ? g_cfg.cfgBurstIntervalMin
                                     : 5;
            assetMs = static_cast<uint32_t>(bmin) * 60'000UL;
        } else {
            const uint8_t hbH = g_cfg.cfgAssetHeartbeatH
                                    ? g_cfg.cfgAssetHeartbeatH
                                    : 24;
            assetMs = static_cast<uint32_t>(hbH) * 3'600'000UL;
        }
        ESP_LOGI(TAG, "asset sleep %u ms (burst=%d)", assetMs, assetBurst);
        prepareSleepMotion(assetMs);
    }
    prepareSleep(sleepMs);
}

} // namespace minilora
