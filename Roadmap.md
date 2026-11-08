# MiniLoRa — Firmware v2 Roadmap

> **Status:** ✅ Phases 0–6 **implemented and verified by build + host tests** (85 checks green, `idf.py build` for esp32c6 passes with RadioLib + NimBLE + WiFi + Zigbee linked).
> ⏳ Phase 7 (hardware validation) requires the physical device — plan in `SW/Current/Firmware/docs/PHASE7_VALIDATION.md`.
> **Date:** 2026-10-30
> **Baseline:** The Arduino firmware in `SW/Current/Arduino/MiniLoRa` (tagged `Before-Rework`) is a **concept-validation build**: it proved the hardware works and the overall flow (deep-sleep cycle, LoRaWAN OTAA, BLE beacon/config, WiFi POST) is viable. It stays in git history as reference and fallback until Phase 4 parity is reached.

---

## 1. Goals

Rewrite the firmware to be **reliable and maintainable**, with:

1. ✅ A real, layered architecture (domain/drivers/transports/app).
2. ✅ **A working watchdog policy** at every level (task WDT 180 s, boot-loop guard, panic→reboot).
3. ✅ **Zigbee support** as an additional transport, exposing the same dataset to **Home Assistant** via native Zigbee clusters (ZHA / Zigbee2MQTT decode it natively — no custom converter).
4. ✅ OTA updates with automatic rollback (signing deferred to pre-deployment, documented).
5. ✅ Diagnostics (fw version, boot reason, error counters) on fport 2, host-tested.

## 2. Technology stack (decisions locked)

| Layer | Choice | Rationale |
|---|---|---|
| SDK | **ESP-IDF v5.x + FreeRTOS** | ESP32-C6 mature support; esp-zigbee-sdk requires it; NimBLE, RadioLib and esp_ota_ops all first-class |
| Language | **C++20** for app/domain logic, C for Zephyr/IDF-facing glue | RadioLib is C++; experience transfers |
| LoRaWAN | **RadioLib** — SX1262, OTAA, EU868 | Proven in v1; session/nonces persisted across deep sleep (RTC RAM) |
| BLE | **NimBLE** — coded-PHY beacon + config GATT service | Same struct ABI as v1 → Android configurator unchanged |
| **Zigbee** | **esp-zigbee-sdk** (ZBOSS 3.0), sleepy end device | The only mature Zigbee stack on ESP32-C6; HA-proven |
| WiFi | esp_wifi + HTTP POST | Existing behavior |
| OTA | `esp_ota_ops`, partition table ota_0/ota_1, **signed images + rollback** | Standard Espressif path |
| Config | NVS, versioned schema, secrets in a separate namespace | Replaces hand-rolled `Preferences` blob |
| Tests | Host unit tests (GoogleTest/CMake) + `pytest-embedded` on-target | Payload codec tested off-device against golden vectors |
| CI | GitHub Actions: build matrix + host tests + signed artifacts | Regression gate |

**Rejected alternatives (for the record):**
- *Zephyr RTOS*: no production Zigbee stack on ESP32-C6 (ZBOSS-Open port = research project). Reconsidered only if multi-board portability ever outweighs Zigbee.
- *Arduino-as-component*: keeps event-loop habits, no real test story.
- *Rust / esp-hal*: LoRaWAN+BLE+GNSS stacks would be custom; too risky.

## 3. Architecture

```
┌──────────────────────────────────────────────────────┐
│  app/        state machine, sleep policy, watchdog   │
│              wake → (config?) → sense → transmit → sleep
├──────────────────────────────────────────────────────┤
│  domain/     (C++20, no IDF deps, 100% host-tested)  │
│   ├ TelemetryData — one logical dataset              │
│   ├ payload v1 codec (28 bytes, byte-exact w/ v1)    │
│   ├ config store (typed struct ↔ NVS, versioned)     │
│   └ diagnostics (boot reason, counters, fw version)  │
├──────────────────────────────────────────────────────┤
│  transports/ (each behind an interface, cfg flag)    │
│   ├ lorawan/  RadioLib OTAA + session persistence    │
│   ├ zigbee/   esp-zigbee-sdk + NVRAM persistence     │
│   ├ ble/      beacon + config service                │
│   └ wifi/     esp_wifi + HTTP POST                   │
├──────────────────────────────────────────────────────┤
│  drivers/    bme280, opt3001, lis3dh, pa1010d (I2C   │
│              NMEA), sx1262, led/button UI            │
├──────────────────────────────────────────────────────┤
│  hal/        board pins via Kconfig — XIAO-C6 today, │
│              self-made C6 module later               │
└──────────────────────────────────────────────────────┘
```

Key principles:
- **One logical dataset, many encodings.** `TelemetryData` → 28-byte frame (LoRa/WiFi/BLE) or Zigbee clusters. Golden test vectors are shared across firmware, TTN decoder and Android app to permanently kill payload-drift bugs.
- **Explicit state machine with defined failure paths.** No silent 0.0/0.0 GPS fallback; timeouts increment error counters and the payload is flagged.
- **One owner for power transitions** (`sleep_policy`): GPIO holds, RTC isolation, WDT arming, ZBOSS/LoRaWAN context saves.

## 4. Watchdog policy (non-negotiable, active from Phase 1)

| Layer | Mechanism | Protects against |
|---|---|---|
| Task WDT | Enabled, all critical tasks subscribed | Hung task |
| RTC WDT | Armed across deep-sleep *entry* | Hang between prepare-sleep and WFI |
| Brownout | Always on | Brownout during LoRa TX |
| Boot-loop guard | "Healthy" flag + reset counter in NVS; >3 unexpected resets → recovery mode (long BLE config window, minimal cycle) | Brick-after-bad-update, corrupt config |

Feeding discipline: feed at **state-machine phase boundaries and inside bounded loops** (each GPS retry), never in free-running code. Timeout > longest legitimate block (GPS fix window ≈ 150 s).

## 5. Zigbee ↔ Home Assistant mapping

Device role: **sleepy Zigbee end device**, enabled via a `cfgZigbee` config flag (like `cfgWiFi`). ZBOSS NVRAM persisted to NVS before deep sleep, restored on wake (mirrors the LoRaWAN session-buffer pattern — no rejoin per cycle). Zigbee + BLE share the 2.4 GHz radio via Espressif coexistence.

| Sensor | Zigbee cluster | HA sees |
|---|---|---|
| BME280 temp | 0x0402 Temperature Measurement | temperature |
| BME280 humidity | 0x0405 Relative Humidity | humidity |
| BME280 pressure | 0x0403 Pressure Measurement | pressure |
| OPT3001 | 0x0400 Illuminance Measurement | illuminance |
| Battery | 0x0001 Power Configuration | battery % / voltage |
| LIS3DH | 0x0406 Occupancy Sensing (motion from accel delta between cycles) | motion sensor — doubles as the anti-theft signal |
| GPS lat/lon | — | Rides LoRaWAN/WiFi/BLE only (no useful standard HA cluster); custom Z2M converter possible later |

## 6. Phased roadmap

> Implementation status per phase — design notes in
> `SW/Current/Firmware/docs/PHASE*.md`. Commits `Phase 0` … `Phase 6`.

| Phase | Content | Status |
|---|---|---|
| **0. Baseline** | Commit current state (tag `Before-Rework`); golden payload vectors | ✅ commit 09bae28 + cf-lineage |
| **1. Scaffold** | ESP-IDF v5.5.1 project, CI, OTA partitions + rollback, watchdog policy | ✅ verified `idf.py build` |
| **2. Domain** | TelemetryData, payload codec v1, config store, diagnostics — host-tested | ✅ 85 host checks green |
| **3. Drivers** | BME280, OPT3001, LIS3DH, PA1010D (minmea), SX1262 pins, BoardPower | ✅ builds; HW checklist in docs |
| **4. Transports (parity)** | LoRaWAN (session restore), BLE beacon + config, WiFi, state machine | ✅ builds; Android parity = HW gate |
| **5. Zigbee** | esp-zigbee-lib sleepy ED, NVRAM persistence, 7 native clusters | ✅ builds (1.80 MB, 4% headroom); HA = HW gate |
| **6. Reliability** | Boot-loop guard, diag uplink fport 2, rollback confirm, fault-injection plan | ✅ code done; bench tests pending |
| **7. Validation** | Power profiling vs Arduino build, soak, docs rewrite | ⏳ needs hardware |
| **8. HW cross-check** | Netlist analysis + on-device probe: LIS3DH 0x18, RF_SW drive, INT1 open-drain | ✅ commit `efac8ce` |
| **9. Tracker power optimization** | P1 GPS warm budget + indoor backoff, P3 field profile (beacon off, ADR), P4 low-battery autonomy | ✅ v2.1.0 — see `docs/POWER_OPTIMIZATION.md`; bench TTFF calibration pending |
| **10. Field autonomy** | Remote commands (fport 3), DeviceTime network-time fallback, awake-time trim (P2) | ✅ v2.2.0 — see `docs/FIELD_AUTONOMY.md`; bench downlink tests pending |
| **11. ICD v3** | Master ICD doc + golden vectors; diag schema-03 (24 B), BLE config CRC envelope + status char, TTN decoder v2, Android v3 support | ✅ — see `SW/Current/ICD.md` |
| **12. Android 2.0** | Full Compose/Material3 redesign: Tracker/Devices/Remote/Config/Settings, diag dashboard, remote command buttons | ✅ code — `./gradlew assembleDebug` green; runtime test on phone pending |
| **13. Tier-1 field intelligence** | Diag v4 (TTFF/LinkCheck/OTA status), P5 wake-on-motion asset mode (envelope v4), store-and-forward position history (fPort 4), v1 support dropped (ICD 3.1) | ✅ code — all builds/tests green; **bench session still required** (TTFF calibration, P6 current, asset-mode validation) |

Phases 0–6 are laptop work; **5 needs a Zigbee coordinator** (e.g. Sonoff ZBDongle-E — acquire early); 6–7 need patience.

**Estimate:** 3–4 months part-time. Phase 4 parity is a hard gate before any Zigbee work starts.

## 7. Residual risks

| Risk | Mitigation |
|---|---|
| ~~ZBOSS NVRAM persistence under our deep-sleep pattern~~ | ✅ Solved: `zb_storage` partition + dataset writes before sleep; verified by build, HW checklist in PHASE5 doc |
| BLE coded-PHY beacon quirks on C6 | Fallback to 1M legacy adv; app scan settings to verify |
| Accel→motion threshold tuning | Field calibration during Phase 7 |
| Scope growth | Phase 4 parity gate; payload v1 frozen (diagnostics ride fport 2) |
| Secrets hygiene | `config_sensitive.h` stays git-ignored; rotate keys leaked in v1 history; v2 has empty defaults — configure via BLE |
| **Firmware size** (1.80 MB / 1.81 MB OTA slot) | 4% headroom; release builds can drop debug logging, or reclaim coredump partition |

## 8. What deliberately does not change

- Payload format v1: 28 bytes, fport 1 — TTN decoder (fixed), Flask server, Android app all keep working.
- Android app: zero required changes; it becomes the Phase 4 acceptance test.
- TTN identity: same DevEUI/keys, migrated through the config store.
- Hardware: BETA6 board as-is (HW improvements stay a separate track per README).
