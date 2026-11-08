# MiniLoRa v2 — Documentation Index

**MiniLoRa** is a small ESP32-C6 tracker board: LoRaWAN + BLE + Zigbee/WiFi,
sensors (BME280, OPT3001, LIS3DH, PA1010D GPS), 1100 mAh 1-cell Li-ion with a
small solar cell, engineered to sleep most of its life and report on a
schedule for **months per charge**.

This folder is the manual. The wire contract lives separately in
[`SW/Current/ICD.md`](../ICD.md) (uplink/downlink/BLE frame layouts); phase
history and design notes are in
[`SW/Current/Firmware/docs/`](../Firmware/docs/).

| Doc | Read it when you want to… |
|---|---|
| **[HowToStart.md](../../../HowToStart.md)** | **bring up a blank device out of the box** — TTN registration, BLE provisioning, first readings |
| [USER_GUIDE.md](USER_GUIDE.md) | **use the Android app** — every tab, workflow, troubleshooting |
| [FLASHING.md](FLASHING.md) | **build + flash** the firmware, back up/restore a device, update it |
| [FIRMWARE.md](FIRMWARE.md) | understand **how the code works** — cycle, sleep, power strategy, recovery |
| [LORAWAN.md](LORAWAN.md) | understand **uplinks & downlinks** — frames, TTN setup, remote commands |
| [ZIGBEE_HA.md](ZIGBEE_HA.md) | integrate with **Zigbee / Home Assistant** |
| [Next-AP-Initial.md](Next-AP-Initial.md) | review the **planned captive-portal** initial-config (app-free unboxing) |

## System at a glance

```
                    ┌─────────────────────────── phone ───────────────────────────┐
                    │  Android app 2.0 (Compose): config, status, TTN view, cmds  │
                    └──────┬──────────────────────────────┬───────────────────────┘
                     BLE config/status/beacon          │ TTN REST API
                           │                           ▼
┌────────────── device ────┴───────┐            ┌──────────────┐
│ wake → sense → transmit → sleep  │──LoRaWAN──▶│ TTN / LNS    │──▶ your server / decoder
│  (ESP32-C6, 15 min … 2 h cycle)  │            └──────────────┘
│  BLE beacon (coded PHY, 28 B)    │──Zigbee───▶ coordinator (HA/Z2M/ZHA)
└──────────────────────────────────┘
```

- **LoRaWAN** is the lifeline: 28-byte telemetry (fPort 1) + 24-byte
  diagnostics (fPort 2) up, remote commands down (fPort 3).
- **BLE** is for provisioning (long-press button), live status, and a coded-PHY
  telemetry beacon readable by the app.
- **Zigbee** is an optional convenience transport into Home Assistant — a
  native HA sensor endpoint, skipped in low-battery states.
- **WiFi** is an optional local-server POST channel.
- **Home Assistant** (no Zigbee needed): MiniLoRa-server publishes every
  device as HA entities via **MQTT discovery** — tracker on the HA map plus
  sensors; see the *Home Assistant integration* section in
  `MiniLoRa-server/README.md`.

## Firmware versions

| Version | Milestone |
|---|---|
| v1 | Arduino POC (tag `Before-Rework`, archived under `archive/`) — payload v1 born here |
| v2.0–2.1 | ESP-IDF rewrite, reliability, tracker power optimization (P1/P3/P4) |
| v2.2 | Field autonomy: remote commands, network time, awake trim |
| v2.3 | ICD v3: diag schema-03, BLE CRC envelope + status characteristic |
| v2.5 | Asset mode (P5), diag schema-04 (TTFF/link margin/OTA status), Zigbee NVRAM |
| v2.6.x | GPS saga solved (RTC anchor, 8-sat gate, 240 s budgets), **remote config** (CFG_SET + diag schema-05 `config_flags`), **SET_RANGE** sleep downlink (ICD 3.6), history backfill, web-UI remote control |

The wire contract — [ICD.md](../ICD.md) — is versioned independently and is
the single source of truth for every frame.

## Repo layout

```
SW/Current/
├── ICD.md               ← THE wire contract (frames, opcodes, CRCs)
├── docs/                ← this manual
├── Firmware/            ← ESP-IDF v5.5.1 project (esp32c6)
│   ├── main/            ← app / domain / drivers / transports
│   ├── host_tests/      ← 96 host-side unit checks + golden vectors
│   └── docs/            ← phase + design docs (POWER_OPTIMIZATION, …)
├── MiniLoRa-server/     ← server: WiFi push + TTN (webhook/poller),
│                          SQLite, graphed web UI, Docker — see its README
├── TTN/                 ← decoder.js, downlink.js, check_vectors.*
└── Arduino/
    └── Android-MiniLoRa/← the app (Compose, Kotlin 2.1)

archive/                ← V1 firmware, OLD-BETA5 hardware, superseded docs
                           (see archive/README.md)
```
