# MiniLoRa — Documentation Index

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
| **[HowToStart.md](../../../HowToStart.md)** | **bring up a blank device out of the box** — pick a radio mode, TTN registration, BLE provisioning, first readings |
| [USER_GUIDE.md](USER_GUIDE.md) | **use the Android app** — every tab (Map/Devices/Remote/Setup), trek, workflows, troubleshooting |
| [FLASHING.md](FLASHING.md) | **build + flash** the firmware, back up/restore a device, update it |
| [FIRMWARE.md](FIRMWARE.md) | understand **how the code works** — cycle, sleep, power strategy, P2P star + trek, recovery |
| [LORAWAN.md](LORAWAN.md) | understand **uplinks & downlinks** — frames, TTN setup, remote commands |
| [ZIGBEE_HA.md](ZIGBEE_HA.md) | integrate with **Zigbee / Home Assistant** |
| [Next-AP-Initial.md](Next-AP-Initial.md) | review the **planned captive-portal** initial-config (app-free unboxing) |
| [ICD §10](../../ICD.md) | the **P2P wire contract** — star + trek frames, security, schedules |

## System at a glance

```
                ┌─────────────────────────── phone ───────────────────────────┐
                │  Android app 3.0 (Compose): Map/Devices/Remote/Setup + help  │
                └──────┬──────────────────────────────┬───────────────────────┘
                BLE config/status/beacon/peer-relay │ TTN REST API
                           │                        ▼
┌────────────── device ────┴───────┐            ┌──────────────┐
│ wake → sense → transmit → sleep  │──LoRaWAN──▶│ TTN / LNS    │──▶ your server / decoder
│  (ESP32-C6, 1 min … 2 h cycle)   │            └──────────────┘
│  BLE beacon (coded PHY, 28 B)    │──Zigbee───▶ coordinator (HA/Z2M/ZHA)
│                                  │──raw LoRa─▶ P2P star gateway (fw 3.0)
│  radio mode (cfgLoRaMode):       │            or trek peer (2 devices,
│   0 = LoRaWAN · 1 = star · 2 = trek           no infrastructure at all)
└──────────────────────────────────┘
```

- **LoRaWAN** is the default lifeline: 28-byte telemetry (fPort 1) +
  diagnostics up to 29 bytes (fPort 2, schema 3/4/5) up, remote commands
  down (fPort 3).
- **P2P star** runs the same payloads over raw LoRa through a
  powered gateway — no TTN, no subscriptions; the gateway forwards to the
  MiniLoRa-server over its own WiFi.
- **Trek** needs no infrastructure at all: two devices exchange
  positions directly every wake; each relays its peer over BLE so **both
  phones map both tracks** — the server is optional.
- **BLE** is for provisioning (long-press button), live status, a coded-PHY
  telemetry beacon, and the trek peer relay — all readable by the app.
- **Zigbee** is an optional convenience transport into Home Assistant — a
  native HA sensor endpoint, skipped in low-battery states.
- **WiFi** is an optional local-server POST channel (also the OTA path).
- **Home Assistant** (no Zigbee needed): MiniLoRa-server publishes every
  device as HA entities via **MQTT discovery** — tracker on the HA map plus
  sensors; see the *Home Assistant integration* section in
  `MiniLoRa-server/README.md`.

## Firmware versions

| Version | Milestone |
|---|---|
| **3.0** (first public release) | **P2P star transport** (gateway/leaves, slot grid, `/p2p` ingest), **trek mode** (two-device bilateral beacon, power ladder, peer-relay BLE adv), schema-5 config envelope (radio mode + P2P fields), AES-CTR/CMAC frame security with boot-anchored IV, remote config (CFG_SET + diag schema-05), SET_RANGE sleep downlink, history backfill, wake-on-motion asset mode, OTA with rollback, 1,920 KB OTA slots |

Everything below the wire contract — [ICD.md](../ICD.md) — is versioned
independently and is the single source of truth for every frame.

## Repo layout

```
SW/Current/
├── ICD.md               ← THE wire contract (frames, opcodes, CRCs, P2P §10)
├── docs/                ← this manual
├── Firmware/            ← ESP-IDF v5.5.1 project (esp32c6)
│   ├── main/            ← app / domain / drivers / transports
│   ├── host_tests/      ← 115 host-side unit checks + golden vectors
│   └── docs/            ← phase + design docs (POWER_OPTIMIZATION, …)
├── MiniLoRa-server/     ← server: WiFi push + P2P-gateway forward + TTN
│                          (webhook/poller), SQLite, graphed web UI, Docker —
│                          see its README
├── TTN/                 ← decoder.js, downlink.js, check_vectors.*
└── Arduino/
    └── Android-MiniLoRa/← the app (Compose, Kotlin 2.1)
```
