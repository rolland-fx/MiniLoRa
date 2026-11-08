# Phase 3 — Drivers

**Status:** ✅ compiles green on ESP-IDF v5.5.1 / esp32c6 — **hardware
validation pending** (needs the board; see "Validation checklist").

## Component choices

| Decision | Choice | Rationale |
|---|---|---|
| BME280 | **Own driver** (`drivers/Bme280.*`) | The registry has no maintained BME280 component; the forced-mode register sequence + datasheet compensation is ~150 lines and dependency-free. Also host-testable in principle later |
| OPT3001, LIS3DH | Own drivers | Trivial register maps; replicate v1 register values byte-for-byte (0xC410 continuous, 0x77/0x10 ctrl) |
| GNSS PA1010D | Own driver + **vendored minmea** | PA1010D is an NMEA stream over I2C, not a register-map sensor. minmea (public-domain-ish) is the standard lightweight NMEA parser. Vendored at `drivers/minmea/` with a documented patch: `timegm()` shim for ESP newlib (`timegm_compat.h`, included via a `#define` at the top of `minmea.c`) |
| I2C | `i2c_master` (new IDF API) with a shared `I2CBus` singleton | One bus, devices added per-address; v1 used the Arduino Wire defaults = **SDA 22 / SCL 23** — confirmed against the official `XIAO_ESP32C6` variant header |

## Pin map (`drivers/Pins.hpp`)

Derived from v1 `config.h`/`MiniLoRa.ino` + the XIAO variant header; the
authoritative cross-check against the schematic is on the validation checklist:

| Signal | GPIO | Note |
|---|---|---|
| SX1262 CS/SCK/MOSI/MISO | 17/19/18/20 | |
| SX1262 RESET/BUSY/DIO1 | 2/1/16 | |
| GNSS wake | 0 | PMTK power control |
| LED blue | 15 | **active LOW** (XIAO builtin) |
| LED green (GPIO6/ext) | 6 | active HIGH |
| Battery enable / sense | 21 / 5 | Vbat/2, 12-bit calibrated |
| LIS3DH INT1 | 4 | unused in v1; reserved for anti-theft wake |
| User button / wake | 7 | EXT1, active LOW |
| I2C | 22/23 | |

**Kept quirk:** v1 read its "BUTTON_2" on **GPIO14**, which is actually the
XIAO's 2.4 GHz antenna-switch select (`WIFI_ANT_CONFIG=14`). Bit 1 of
gpioStatus keeps reading GPIO14 for payload parity; the *real* button (GPIO7)
drives wake/config. Documented in `Pins.hpp` as `kButton2Legacy`.

## Behavioral parity notes

- Sensor sampling mirrors v1: BME 4×, OPT3001 4×(120 ms each), LIS3DH 8×,
  battery 8× (200 ms settle + 100 ms spacing).
- GPS: `GPS_MIN_SATELLITES = 7`; 60 iterations after wake, 150 on cold boot
  (v1 semantics); PMTK220,200 mid-power on wake, PMTK225,4 standby before
  sleep, wake pin dropped.
- Battery: divider enabled only during the read (`BAT_ON` low afterwards) —
  v1 power discipline preserved.
- **v2 explicit failure path:** sensor read failures and GPS timeouts bump
  `Diagnostics` counters; a GPS timeout sends the `no_fix` payload shape
  (zeros + sat 0) instead of v1's silent stale/zero coordinates.

## Time handling

v1 built the epoch from GPS time via `ESP32Time` (TZ-sensitive). v2 uses a
pure `utcToEpoch()` in the domain layer (Hinnant civil-days algorithm) —
exactly UTC, host-tested.

## Validation checklist (on hardware, Phase 4 gate)

- [ ] I2C: all 4 addresses ACK (0x10, 0x19, 0x76, 0x47)
- [ ] BME280 chip ID 0x60; sanity: 15–35 °C, 950–1050 hPa
- [ ] OPT3001 lux plausible indoor/outdoor
- [ ] LIS3DH ±1 g on each axis when tilted
- [ ] Battery mV within ±50 mV of a multimeter
- [ ] GPS fix under open sky < 60 s warm
- [ ] GPIO14 reads stable (antenna switch state)
