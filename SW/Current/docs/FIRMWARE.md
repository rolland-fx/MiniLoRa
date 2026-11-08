# Firmware Architecture — How the Code Works

Source: `SW/Current/Firmware/` (ESP-IDF v5.5.1, C++20, FreeRTOS).
Layering is strict — **domain** (pure logic, host-testable) · **drivers**
(hardware) · **transports** (radios) · **app** (the state machine):

```
main/
├── app_main.cpp          entry: boot-loop guard → runCycle() → deep sleep
├── app/
│   ├── App.cpp           THE state machine (one full wake→sleep cycle)
│   ├── Sleep.{hpp,cpp}   deep-sleep prep: RTC GPIOs, holds, wake sources
│   └── Stores.{hpp,cpp}  NVS persistence (config "cfg", diagnostics "diag")
├── domain/               pure logic — no ESP calls, unit-tested on the host
│   ├── Payload.{hpp,cpp}        28 B telemetry v1 codec (FROZEN)
│   ├── DiagPayload.{hpp,cpp}    24 B diagnostics schema-03 codec
│   ├── Config.{hpp,cpp}         DeviceConfig blob + v3 CRC envelope
│   ├── Diagnostics.hpp          counters, flags, golden-vector types
│   └── Telemetry.hpp            TelemetryData + sleep table
├── drivers/              I2CBus, BoardPower (ADC/LEDs), Bme280, Opt3001,
│                         Lis3dh, Pa1010d (+minmea), Pins.hpp (netlist truth)
└── transports/
    ├── lorawan/          RadioLib SX1262 + LoRaWANNode, OTAA EU868
    ├── ble/              NimBLE: beacon, config service, status characteristic
    ├── zigbee/           esp-zigbee sleepy end device, native HA clusters
    └── wifi/             HTTP POST to a local server
```

## The cycle (the whole lifetime is one function)

`app_main` → `runCycle()` runs **one complete cycle and never returns** — it
ends in deep sleep. There is no idle loop, no RTOS housekeeping between
cycles; RAM is discarded, and everything that must survive lives in
**RTC RAM** (`RTC_DATA_ATTR`) or **NVS**.

```
wake (timer / EXT1 button / power-on)
  ├─ boot: watchdog on, BoardPower init, ConfigStore::initAndLoad
  ├─ diag: DiagStore::onBoot (boot counters, boot-loop guard)
  ├─ OTA: confirm PENDING_VERIFY image after a healthy load
  ├─ config mode? (button ≥ 3 s) → BLE config window → sleep
  ├─ recovery mode? (>3 unexpected resets) → minimal cycle → sleep
  ├─ remote commands applied (fPort 3 flags from last cycle's downlink)
  ├─ power strategy (below) → decide GPS/budget
  ├─ sense: battery → BME → LIS3DH → OPT3001 → GPS (budgeted window)
  ├─ build payload v1 + optional diag frame (schema-03)
  ├─ BLE beacon (follows the runtime cfgBLE flag / app toggle)
  ├─ LoRaWAN: join-or-restore → uplink (RX1/RX2 open) → apply downlink
  │           (+ diag uplink every 16 boots or on demand)
  ├─ WiFi POST (optional) · Zigbee report (optional)
  └─ DiagStore::save → prepareSleep(interval) → esp_deep_sleep_start()
```

### Power strategy (the interesting part)

Decided **before** sensing, in `runCycle`:

| Input | Decision |
|---|---|
| Cold boot (power-on) | GPS budget = 150 s |
| Warm wake (RTC timer) | GPS budget = `MINILORA_GPS_WARM_ATTEMPTS` (default **15 s**) — ephemeris survives in the GPS backup mode, so TTFF is seconds |
| `gpsConsecFails ≥ 1` (NVS) | **indoor backoff**: GPS runs only every `2^min(fails,3)` cycles (up to every 8th); a fix clears the counter |
| `batMv < 3500` | degraded: no beacon/Zigbee/WiFi, GPS every 2nd cycle, **sleep ×2** (≤ 120 min) |
| `batMv < 3300` | critical: no GPS, BME only, LoRaWAN heartbeat, sleep 120 min |
| `LOCATE_NOW` downlink | forces a GPS window (never in critical) + next sleep = 60 s |

The battery read itself was trimmed to ~0.26 s (P2), sensor oversampling is
halved — the awake window is dominated by GPS, which is why the warm budget
and backoff are the levers that matter
(details: `Firmware/docs/POWER_OPTIMIZATION.md`).

### Asset mode (P5, config schema 4)

`cfgAssetMode` turns the tracker into a long-idle asset tag: it sleeps until
**motion** (LIS3DH INT1, active-low open-drain on the shared GPIO4 net, as a
second EXT1 source alongside the button) or the **heartbeat timer**
(`cfgAssetHeartbeatH`, default 24 h). A motion wake forces a warm GPS window
and reports at `cfgBurstIntervalMin` (default 5 min), capped at
`cfgBurstMaxPerDay` (default 20) motion-GPS bursts per net-time day. Idle
draw is the ~30–45 µA sleep floor — years on the 1100 mAh cell. Positions
during GPS-less periods ride the fPort-4 history frame.

### Time without GPS

When a cycle has no GPS time, the uplink queues a **DeviceTimeReq** MAC
command; the answer + following sleep are kept in RTC RAM, so later cycles
estimate UTC to a few seconds (never accumulating — every fallback cycle
re-requests). See `Firmware/docs/FIELD_AUTONOMY.md`.

## Reliability

- **Task watchdog** fed through the cycle (`feedWatchdog()`); a hang resets.
- **Boot-loop guard**: `DiagStore::onBoot` counts unexpected resets
  (anything but deep-sleep/power-on/software); > 3 → recovery mode. A clean
  cycle clears the counter.
- **OTA rollback**: `esp_ota_mark_app_valid_cancel_rollback()` after a
  healthy boot; unconfirmed images fall back to the other slot
  (see [FLASHING.md](FLASHING.md#rollback--boot-loop-protection-automatic-no-user-action)).
- **OTA pull (FW ≥ 2.5.0)**: `app/Ota.cpp` streams the image from the
  MiniLoRa-server into the passive slot and SHA-256-verifies it before
  switching boot — the running image is never touched on any failure.
- **Failure paths never block**: every sensor/transport failure increments a
  counter in the diag frame and the cycle continues — a tracker that arrives
  home *empty* beats one that silently stopped.

## Persistence model

| What | Where | Survives |
|---|---|---|
| `DeviceConfig` (159 B + schema byte) | NVS `config`/`cfg` | power cycle, reflash (not `erase_flash`) |
| Diag counters (`DiagStore`) | NVS `diag` | same |
| LoRaWAN nonces/session | RTC RAM | deep sleep only — a power cycle rejoins once |
| GPS backup (ephemeris) | the PA1010D's own backup rail (15 µA) | deep sleep |
| Zigbee network context | `zb_storage` FAT partition (ZBOSS NVRAM) | power cycle |
| Last accel vector, command flags, net epoch, burst counters | RTC RAM | deep sleep |
| Position history (8 fixes) | NVS `hist` | power cycle |

## BLE details (also see ICD §4–5)

- **Config service** `0000FACE…` / `0000CAFE…`: opens on long-press (120 s)
  or via the `BLE_WINDOW` command. Read **and** write use the same
  **163-byte envelope** (`'C'`, schema 3, 159-byte body, CRC16-CCITT) —
  legacy 158/159-byte blobs are rejected. The **status characteristic**
  `0000CAFF…` returns a fresh 16-byte frame per read (real-mV battery
  re-measured then).
- **Telemetry beacon**: extended advertising, LE **coded PHY**, 625 ms
  interval, manufacturer data = payload v1 + company id. Follows the
  runtime `cfgBLE` flag — toggle it in the app's Provision tab (the old
  `MINILORA_DEFAULT_BEACON` compile-time gate was removed 2026-11-01).

## Hardware notes worth knowing

- `drivers/Pins.hpp` is the netlist truth; LIS3DH is at **0x18** (v1's 0x19
  was wrong), OPT3001 0x47, BME280 0x76, GPS 0x10.
- `LORA_RF_SW` (GPIO3): HIGH = RX path; DIO2 drives TX internally — handled
  by `setDio2AsRfSwitch(true)` + explicit GPIO3 drive in the LoRaWAN
  transport. v1 never enabled RX — that's fixed.
- `PPS_INT` (GPIO4) is shared by GPS PPS, LIS3DH INT1 (open-drain) and
  OPT3001 INT with **no pull-up** — never arm it as a wake source while the
  GPS is awake.
- Battery divider is behind Q1 (no sleep drain), read via calibrated ADC
  (eFuse curve fitting).

Full board analysis incl. probe results: `Firmware/docs/HW_CROSSCHECK.md`.
