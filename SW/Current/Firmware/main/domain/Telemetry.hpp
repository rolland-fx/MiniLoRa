// One logical dataset (Roadmap §3): every transport serializes THIS struct.
#pragma once

#include <cstdint>

namespace minilora {

// Sleep interval table — index stored in config and set by downlink.
// Mirrors SLEEP_T[] from the v1 firmware (index 0..8).
inline constexpr uint32_t kSleepTableMs[] = {
    60'000,     120'000,    300'000,    600'000,    900'000,
    1'200'000,  1'800'000,  3'600'000,  7'200'000,
};
inline constexpr uint8_t kSleepTableLen =
    sizeof(kSleepTableMs) / sizeof(kSleepTableMs[0]);

/// Byte 26 of the payload carries sleep *minutes* (v1 semantics).
inline uint8_t sleepMinutes(uint8_t idx)
{
    return static_cast<uint8_t>(
        kSleepTableMs[idx < kSleepTableLen ? idx : 0] / 60'000);
}

/// Best-effort reverse of sleepMinutes(); returns 0 (1 min) when unknown.
inline uint8_t sleepIndexFromMinutes(uint8_t minutes)
{
    for (uint8_t i = 0; i < kSleepTableLen; ++i) {
        if (sleepMinutes(i) == minutes) {
            return i;
        }
    }
    return 0;
}

/// UTC → unix epoch (Howard Hinnant's civil-from-days inverse).
/// Pure function — host-tested; no TZ dependence (v1 used ESP32Time with
/// an implicit TZ assumption; v2 is exact UTC).
inline uint32_t utcToEpoch(int year, int month, int day, int hour, int minute,
                           int second)
{
    long long y = year;
    unsigned m = static_cast<unsigned>(month);
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    return static_cast<uint32_t>(
        days * 86400 + hour * 3600 + minute * 60 + second);
}

struct TelemetryData {
    // Environmental
    int8_t   tempC;        // °C
    uint8_t  humPct;       // %RH
    uint16_t presDeciHpa;  // 0.1 hPa units (payload bytes 2-3; 10300 = 1030.0 hPa)
    uint16_t batMv;        // millivolts
    uint16_t lux;

    // LIS3DH raw counts (±4 g ≈ ±8190 counts at 12-bit high-res)
    int16_t  acc[3];

    // GNSS
    int32_t  latMicro;     // degrees × 1e6
    int32_t  lonMicro;     // degrees × 1e6
    int16_t  altM;         // meters
    uint8_t  sats;
    uint32_t epochSec;     // fix time, 0 = no fix
    bool     gpsValid;

    // Housekeeping
    uint8_t  awakeSec;     // seconds since wake (clamped to 255)
    uint8_t  sleepIdx;     // index into kSleepTableMs (0..8)
    uint8_t  gpioStatus;   // bit0 = GPIO6/LED_GREEN, bit1 = button_2
};

static_assert(sizeof(TelemetryData) >= 28, "payload contract needs >= 28 bytes of data");

} // namespace minilora
