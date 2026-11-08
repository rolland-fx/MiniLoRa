# Phase 5 — Zigbee Transport

**Status:** ✅ compiles + links green (esp-zigbee-lib 1.6.8 + esp-zboss-lib on
ESP32-C6, coexisting with NimBLE + RadioLib). **HA/Z2M end-to-end validation
pending hardware** — see checklist.

## What was built

`transports/zigbee/ZigbeeTransport.{hpp,cpp}` — a **sleepy Zigbee end device**
(`ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID`, ep 1) exposing the MiniLoRa dataset as
**native HA clusters** — Z2M/ZHA auto-discover everything, no converter:

| Attribute | Cluster | Encoding |
|---|---|---|
| `t.tempC` | 0x0402 Temp Measurement | int16 ×100 °C |
| `t.humPct` | 0x0405 Relative Humidity | uint16 ×100 % |
| `t.presHPa` | 0x0403 Pressure | int16 hPa |
| `t.lux` | 0x0400 Illuminance | uint16 lux |
| accel delta | 0x0406 Occupancy Sensing | 0/1 — **motion sensor** |
| `t.batMv` | 0x0001 Power Config | voltage 100 mV units + % 0.5-step |

`cfgZigbee` (the 159th config byte, Phase 2) gates the whole transport —
off by default for legacy-configured devices.

## Technical decisions & hard-won facts

### 1. Version pinning: use the 1.6.8 classic API
esp-zigbee-sdk's `master` examples use a **new "ezb_" API that isn't released**
— esp-zigbee-lib 1.6.8 (latest at the time of writing) exposes the classic
`esp_zb_*` API. The transport is written against the installed headers, not
the docs/examples. Transitive dep quirk: `esp-zboss-lib` must be listed
explicitly in `idf_component.yml` — the resolver missed it.

### 2. NVRAM persistence (the roadmap's #1 Zigbee risk — solved)
- ZBOSS NVRAM on ESP32 targets lives in a **flash partition named
  `zb_storage`** (verified in the prebuilt library strings) — added to
  `partitions.csv` (128 KB, data/fat subtype).
- `persist()` writes `ZB_NVRAM_COMMON_DATA` + `ZB_NVRAM_HA_DATA` datasets
  **before deep sleep**; `esp_zb_nvram_erase_at_start(false)` keeps it.
- On wake, `ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT` fires and the signal handler
  sets joined without steering — the device **does not rejoin per cycle**.
- The `zb_storage` partition itself survives deep sleep (it's flash); NVRAM
  writes go through ESP-IDF wear levelling.

### 3. Child aging vs deep sleep
A ZED that sleeps 15–120 min must not be aged out by its parent:
`ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_8192MIN` (≈5.7 days). `keep_alive`
30 s covers the awake window; data request polls happen while the stack runs.

### 4. Motion from accelerometer (roadmap §5 bonus)
`publish()` compares `TelemetryData.acc` against the **previous cycle's**
vector (kept in `RTC_DATA_ATTR g_prevAcc`) — delta > 400 counts (≈0.2 g) ⇒
Occupancy = 1. This is the anti-theft signal usable directly in HA
automations, and it costs zero extra hardware.

### 5. Cycle-time model
`startAndJoin(30 s)` — instant when NVRAM context exists (stack restores,
`DEVICE_REBOOT` signal), full steering only when factory-new or evicted.
`publish()` sets attributes + sends 6 report commands to the coordinator.
`persist()` then NVRAM write, and the whole heap dies at deep sleep.

### 6. Radio coexistence
The C6 has separate 802.15.4 and BLE radios + an Espressif coexistence layer.
Zigbee (802.15.4), BLE beacon (BLE radio) and WiFi share the awake window;
the stack's own arbitration handles it. Validated only on hardware (checklist).

## Exit checklist (hardware + HA)

- [ ] `zb_storage` partition flashed; NVRAM persists across ≥10 sleep cycles
      (no rejoin in Z2M logs after the first)
- [ ] Device pairs with Sonoff ZBDongle-E; all 7 clusters appear in Z2M
- [ ] HA sees: temp, humidity, pressure, illuminance, battery, motion
- [ ] Motion flips to occupied when the board is shaken between cycles
- [ ] BLE beacon + LoRaWAN + Zigbee enabled simultaneously in one cycle
- [ ] Deep-sleep current unchanged with `cfgZigbee` on (radio parked)
