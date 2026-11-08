# Phase 7 — Field Validation & Documentation

**Status:** 📋 plan ready — every remaining item needs the physical device.

## 1. First-flash bring-up (30 min)

1. `idf.py -p /dev/ttyACM0 flash monitor` — watch the boot log:
   `MiniLoRa v2.0.0-dev boot`, config defaults loaded (no NVS yet).
2. Confirm I2C probes: BME280 id 0x60, LIS3DH id 0x33, OPT3001 ACK, GPS ACK
   (see checklist in `PHASE3_DRIVERS.md`).
3. Long-press button → blue LED blinks → configure via the **stock v1 Android
   app** (BLE write = 158 bytes, accepted by design).

## 2. Parity acceptance (the Phase 4 gate)

Run both firmwares side by side for a day:
- Android app decodes the v2 coded-PHY beacon (values match the v1 device).
- TTN console: v2 joins OTAA, uplinks decode with the (fixed) decoder.js.
- 10 sleep cycles → **no rejoin** in TTN logs (session restored from RTC RAM).
- Downlink sleep-index change applies + survives power cycle (NVS).
- WiFi POST arrives at `Wifi-Test-Server/server.py`.
- Zigbee: pair with the coordinator, all 7 clusters visible in Z2M (Phase 5).

## 3. Power profiling

| Measurement | Method | Target |
|---|---|---|
| Deep-sleep current | µCurrent/Joulescope on battery | ≤ v1 + 10% (Zigbee NVRAM adds nothing in sleep; radio parked) |
| Awake current | log over USB while bench supply meters | comparable to v1 |
| Cycle duration | `awakeSec` byte in payload | ≤ v1 (GPS window dominates) |
| Battery life | 2-week soak vs v1 baseline | ≥ v1 |

Watch-outs found during development:
- ZBOSS keeps a task + esp_timers alive during the awake window — verify it
  parks before `esp_deep_sleep_start` (the `persist()` delay covers the
  NVRAM flash write).
- The BLE beacon now runs at 625 ms intervals (16× v1) — measure its share.

## 4. Reliability fault-injection

Execute the matrix in `PHASE6_RELIABILITY.md` (hang task, bad image, panic,
brownout, NVS corruption). Then **enable secure boot v2 signing** per the
deferred-signing procedure before any outdoor deployment.

## 5. Documentation updates (repo)

- [ ] `README.md` — rewrite Software Overview: v2 stack, build instructions,
      config flow, Zigbee/HA section.
- [ ] `SW/Current/Firmware/README.md` — already current; add flash diagram.
- [ ] Move the Arduino reference to `SW/Backup/Arduino-v1` once parity is
      proven (git history keeps everything anyway).
- [ ] `Roadmap.md` — mark phases complete.

## 6. Known follow-ups (post-v2)

- Android app: add the 159-byte write path (Zigbee toggle) — optional, the
  firmware accepts both sizes.
- `decoder.js`/server: no change needed (payload v1 frozen).
- GPS altitude from GGA vs barometric fusion — nice-to-have.
- Anti-theft mode: LIS3DH INT1 → EXT1 wake (pin wired, driver stub exists).
- Multi-board: second `pins_*.hpp` for the self-made C6 module.
