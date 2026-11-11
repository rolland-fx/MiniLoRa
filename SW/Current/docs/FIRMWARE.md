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
│   ├── Stores.{hpp,cpp}  NVS persistence (config "cfg", diagnostics "diag")
│   ├── P2pGateway.{hpp,cpp}  P2P star gateway: awake-learn → slot grid,
│   │                         forward leaves' frames to the server
│   ├── P2pTrek.{hpp,cpp}     P2P trek: bilateral exchange, power ladder,
│   │                         peer-relay BLE advertisement
│   └── Ota.{hpp,cpp}     server-pull OTA (stream + SHA-256 + rollback)
├── domain/               pure logic — no ESP calls, unit-tested on the host
│   ├── Payload.{hpp,cpp}        28 B telemetry v1 codec (FROZEN)
│   ├── DiagPayload.{hpp,cpp}    24/28/29 B diagnostics codec (schema 3/4/5)
│   ├── Config.{hpp,cpp}         DeviceConfig blob + CRC envelopes (schema 3/4/5)
│   ├── P2pFrame.{hpp,cpp}       §10.3 P2P frame: header + CTR + CMAC + CRC
│   ├── P2pSchedule.{hpp,cpp}    GPS-anchored slot grid (data/catch slots)
│   ├── RtcClock.hpp             GPS-anchored wall clock (shared by P2P modes)
│   ├── Diagnostics.hpp          counters, flags, golden-vector types
│   └── Telemetry.hpp            TelemetryData + sleep table
├── drivers/              I2CBus, BoardPower (ADC/LEDs), Bme280, Opt3001,
│                         Lis3dh, Pa1010d (+minmea), Pins.hpp (netlist truth)
└── transports/
    ├── lorawan/          RadioLib SX1262 + LoRaWANNode, OTAA EU868
    ├── p2p/              RadioLib SX1262 raw: CAD, TX/RX, private sync word
    ├── ble/              NimBLE: beacon, peer-relay adv, config service,
    │                     status characteristic
    ├── zigbee/           esp-zigbee sleepy end device, native HA clusters
    └── wifi/             HTTP POST to a local server (per-frame or batched)
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
  ├─ ── radio mode branches (cfgLoRaMode) ──
  ├─ LoRaWAN (mode 0): join-or-restore → uplink (RX1/RX2) → apply downlink
  │           (+ diag uplink every 16 boots or on demand)
  ├─ P2P leaf (mode 1): TX in the hash-assigned slot + 2 s command window
  ├─ P2P gateway (mode 1): learn phase or catch-slot grid → forward frames
  ├─ P2P trek (mode 2): GPS-anchored bilateral exchange → peer-relay adv
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

### P2P modes (FW ≥ 3.0, `cfgLoRaMode` 1–2)

One air interface: selecting a P2P mode suspends the LoRaWAN phase entirely
(§10.10). Both modes share a **private sync word** (0x12 — invisible to
LoRaWAN networks), the §10.3 frame format (AES-128-CTR body + AES-CMAC MIC,
IV = origin‖seq‖boot — a power cycle re-seeds the counter from the RNG so
keystreams never repeat), and the **GPS-anchored clock** (RtcClock.hpp —
the window/slot grids key on wall time, never on the boot-relative timer,
so two devices' grids always meet).

- **P2P star (`cfgLoRaMode = 1`)** — one powered **gateway**, N battery
  **leaves**. Leaves TX in hash-assigned slots of a 60 s superframe
  (profile-dependent: SF12 → 3 s slots, SF10 → 800 ms, SF7 → 130 ms) plus
  an optional 2 s command window. The gateway boots into a 30-min
  **awake-learn** phase (continuous RX: learns each leaf's id, cadence and
  phase, forwards immediately), then lives on the **slot grid**: catch
  slots every 60 s + predicted leaf windows, deep-sleeping between. Every
  heard frame is deduplicated and forwarded to the server under the leaf's
  own id (`POST /data` for telemetry, `/diag` for diagnostics). The
  gateway's own telemetry rides its own hash slot.
- **P2P trek (`cfgLoRaMode = 2`)** — two devices, nothing else. Windows
  anchor to the GPS epoch (`window ≡ 0 mod period`, period = the sleep
  table); each window has three sub-slots (S0 lower-id TX, S1 higher-id
  TX, S2 retry). Roles derive from the short ids; during bootstrap the
  role draws fresh from the RNG each window (~98% contact within 3
  windows). A **4-step power ladder** (14→17→20→22 dBm, legal on
  869.525 MHz) adapts per device: miss or RSSI < −115 dBm steps up, three
  strong frames step down. The pre-window gap is spent in **light sleep**
  (~150 µA — RAM, clock and the built frame all survive). After 3 missed
  windows the green LED triple-double-blinks at each wake. Each device
  then relays the **peer's** last frame over BLE (§10.9 advertisement) so
  both phones draw both tracks.

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
| `DeviceConfig` (189 B + schema byte, NVS schema 5) | NVS `config`/`cfg` | power cycle, reflash (not `erase_flash`) |
| Diag counters (`DiagStore`) | NVS `diag` | same |
| LoRaWAN nonces/session | RTC RAM | deep sleep only — a power cycle rejoins once |
| GPS backup (ephemeris) | the PA1010D's own backup rail (15 µA) | deep sleep |
| Zigbee network context | `zb_storage` FAT partition (ZBOSS NVRAM) | power cycle |
| Last accel vector, command flags, net epoch, burst counters | RTC RAM | deep sleep |
| P2P: trek contact state + power level, gateway learn table + dedup ring, `p2pSeq` frame counter | RTC RAM | deep sleep (a power cycle re-seeds `p2pSeq` from the RNG — IV-reuse guard) |
| Position history (8 fixes) | NVS `hist` | power cycle |
| Store-and-forward buffer (50 frames) | NVS `buf` | power cycle |

## BLE details (also see ICD §4–6, §10.9)

- **Config service** `0000FACE…` / `0000CAFE…`: opens on long-press (120 s)
  or via the `BLE_WINDOW` command. Read **and** write use the same
  CRC-protected envelope — **schema 5 is current** (`'C'`, 5, 189-byte body,
  CRC16 = 193 B; schema 3/4 remain accepted for old clients; legacy
  158/159-byte blobs are rejected). The **status characteristic**
  `0000CAFF…` returns a fresh 16-byte frame per read (real-mV battery
  re-measured then).
- **Telemetry beacon**: extended advertising, LE **coded PHY**, 625 ms
  interval, manufacturer data = payload v1 + company id. Follows the
  runtime `cfgBLE` flag — toggle it in the app's Setup tab (the old
  `MINILORA_DEFAULT_BEACON` compile-time gate was removed 2026-11-01).
- **Peer-relay beacon (trek, §10.9)**: a second advertisement carrying the
  peer's last frame (`0x02`-typed manufacturer data: origin + seq + 28 B
  body) — the app draws the peer's track and its contact state from it.

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
