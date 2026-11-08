# Android App — User Guide

App version 2.0 (Jetpack Compose, Material 3). Requires Android 13+ (minSdk 33).
The APK is built from `SW/Current/Arduino/Android-MiniLoRa`
(`./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`).

> Bringing up a **blank device** for the first time? Start with
> [HowToStart.md](../../../HowToStart.md) — this guide covers every tab in
> depth.

## Permissions (first launch)

The app asks for **BLUETOOTH_SCAN**, **BLUETOOTH_CONNECT** and
**ACCESS_FINE_LOCATION**. Grant all three — scanning and the config service
need them. It will also ask to enable Bluetooth; accept.

## The four tabs

### 1 · Tracker (home)

Everything about the *data*, not the radio.

- **Status header** — the newest payload: temperature, humidity, coordinates
  (or `GPS: no fix`), battery voltage, satellite count, a source chip
  (`BLE`, `TTN`, `WiFi`, …) telling you which channel delivered it, and how
  **old** it is (`just now`, `4 min ago`, …). If the device has been
  configured over BLE at least once, a second line shows its live status:
  `fw 2.6.14 · bat 4021 mV · joined=true · gpsFails=0` — including warnings
  like `[LOW BAT]` or `[RECOVERY]`.
- **Map** — osmdroid/OpenStreetMap with the last 10 valid positions; the
  newest is the opaque marker. Needs internet for tiles.
- **History** — swipeable strip of recent payloads (newest right). Each card
  shows temp/hum/pressure, time, source and coordinates.

*Data sources:* the **Scan** tab (BLE beacon reception) and the **Remote**
tab (TTN/WiFi polling) both feed this screen — and every TTN/WiFi pull also
**backfills the last ~10 frames** from the network, so the history covers
cycles that arrived while the app was closed. History is persisted on the
phone and survives restarts.

### 2 · Scan

The beacon scanner. Press scan (or just wait) — the tracker's BLE beacon
(coded PHY, every 625 ms) is parsed and only devices with a **valid MiniLoRa
frame** are listed, with RSSI and last-seen. **Tap a device to select it** as
the target for configuration and status reads. The selected device is marked ✓.

> The beacon is a telemetry transmitter, not a config advertisement — you do
> *not* need to "connect" to it. Provision (Provision tab) is a separate GATT
> service the device opens when you long-press its button.

### 3 · Remote

Everything that reaches the **device**, in three cards. Requires TTN
credentials in Settings — commands travel as TTN downlinks; nothing here
touches the phone's radios.

- **Commands** — the five remote actions (queued via the TTN downlink API,
  applied at the start of the device's next wake cycle):
  | Button | Effect on the tracker |
  |---|---|
  | **Locate now** | forces a GPS window next cycle (bypasses the indoor backoff and low-battery GPS skip) and reports again after 60 s |
  | **Send diag** | sends a diagnostics frame next cycle |
  | **Reset counters** | clears all failure counters on the device |
  | **Open BLE window** | opens the BLE config service for 120 s on next wake — *remote* provisioning: you can walk up to a mounted tracker and reconfigure it without touching the button |
  | **Update firmware** | the device pulls the active image from the MiniLoRa-server on its next wake (see the server's /ota page) |

  One line under the buttons estimates when the last command takes effect
  (from the device's current sleep interval).

- **Downlink queue** — every command you queued, with its frame bytes, the
  queue time and its state: `pending` (riding the next wake cycle's RX
  window) → `sent`; a newer command supersedes a still-pending one
  (`replaced`). While anything is pending the card refreshes itself every
  30 s, and a **Clear TTN downlink queue** button cancels all pending
  downlinks device-wide (including ones queued from the server or the
  console). Completed commands clear after 30 minutes.

- **Diagnostics** — firmware version, boot count, GPS/LoRa/WiFi/Zigbee/
  sensor failure counters, unexpected resets, consecutive-GPS-fail backoff
  counter, power flags (LOW BATTERY / CRITICAL), OTA-pending and
  recovery-mode flags, **and when the frame was received**. The displayed
  frame persists across restarts until a newer one arrives.

- **Remote config** — the transport/sensor switches and the sleep duration,
  mirrored from the device's latest diagnostics frame and changeable over
  the air (ICD 3.5/3.6): **WiFi, Zigbee, BLE beacon, GNSS, BME280, OPT3001,
  LIS3DH** — each with ON/OFF (disabled when already in that state) — and a
  **Sleep duration** dropdown (1 min … 120 min). Changes persist on the
  device; the confirm arrives on the next diag frame (for sleep: the next
  telemetry frame's sleep-minutes byte). LoRaWAN is always on, and the
  long-press BLE config always works — these toggles can never lock you out.

### 4 · Provision

The device editor. Requires a **selected device** (Scan tab) and the
tracker sitting in its **BLE config window** — hold the button ≥ 3 s (the
device wakes with a 120 s window). Then:

- **Read** — chains a status read (battery, counters, joined) and a config
  read; the editor fields are populated from the device.
- **Write to device** — builds the 164-byte body from the fields and sends it
  inside the ICD v4 CRC-protected envelope. A corrupted write is rejected by
  the device, never half-applied.
- **Export .cfg** — saves the editor state as the device's native 168-byte
  envelope file (via the Android file picker, e.g. `minilora-<name>.cfg`).
  Read first to mirror the device; the file contains the TTN keys and WiFi
  password in clear text — treat it like a credential.
- **Import .cfg** — pick a `.cfg` file (generated by the app itself or the
  server's **Config builder** page); it is parsed and CRC-validated, the
  editor fills with its values — review, then **Write to device**.
  Nothing is written until you press Write. This is the device-cloning
  path: Read A → Export → Import on B → Write.
- **Save** (implicitly on Read/Write/Export/Import) — keeps everything in
  encrypted prefs.

Fields: device name, WiFi SSID/password/server IP + port (optional WiFi
transport), JoinEUI/DevEUI/AppKey/NwkKey (LoRaWAN OTAA), transport toggles
(LoRaWAN, WiFi, GNSS, BME280, OPT3001, LIS3DH, BLE beacon, Zigbee), the
sleep duration slider (1 min … 120 min), and the **Asset mode (P5)** block:

- **Wake on motion** — the tracker sleeps until the accelerometer detects
  movement (or the heartbeat timer). A motion wake forces a GPS fix and a
  report, then it re-reports at the burst interval for as long as motion
  keeps triggering (capped per day); between bursts it sends cheap
  heartbeats carrying the last known position.
- **Sensitivity** Low/Med/High ≈ 128/256/512 mg; **Heartbeat hours**,
  **Burst minutes** and **Bursts/day** tune the schedule.

Typical asset settings: Med sensitivity, 24 h heartbeat, 5 min bursts,
20/day — years of idle battery life.

**About GPS-less cycles:** when the tracker can't see the sky it still
reports its **two most recent fixes** (fPort 4) — the server plots them
tagged `ttn-hist`, so an underground day still draws a track.

> **Secrets stay local.** Keys are stored in EncryptedSharedPreferences and
> are only ever sent over BLE — never via TTN downlink.

### Settings (gear, top right)

TTN API key / app ID / device ID / region (eu1, nam1…) / poll interval; the
local WiFi server IP + poll interval; the target-device name/MAC filters
used by the scanner and the TTN device-ID fallback; and the **Polling**
switches (TTN / WiFi / BLE) — these are *phone* behavior (which feeds the
app listens to), remembered across restarts and resumed on launch.
Everything is stored in encrypted prefs.

- **Export settings** — saves all of the above as a versioned JSON file
  (`minilora-app-settings.json`) via the Android file picker: clone the app
  setup to another phone, or back it up before reinstalling.
- **Import settings** — pick a settings file; it is validated (type +
  version), merged into the current settings and saved. Unknown keys are
  ignored (forward-compatible). This never touches the tracker — device
  configuration travels separately in the Provision tab (.cfg).

> The settings file contains the **TTN API key in clear text** — treat it
> like a credential.

## Typical workflows

**Provision a fresh tracker**
1. Flash it (see [FLASHING.md](FLASHING.md)), hold the button ≥ 3 s.
2. Scan tab → select it. Provision tab → fill JoinEUI/DevEUI/keys from your
   TTN console → **Write**.
3. Done — it rejoins and reports on the next cycle. (TTN → device → check
   "Latest uplink".)

**Mount it and forget it**
1. Enable Zigbee in Config only if you run a coordinator (optional).
2. Remote tab → enable TTN polling (phone) and note the diag cadence
   (~every 16 boots).
3. Later: Remote → **Locate now** to find it, **Send diag** to check its
   health from the couch.

**Reconfigure without touching it** (mounted high, waterproof box)
1. Remote → **Open BLE window** → within its next wake cycle the device
   advertises the config service for 120 s.
2. Walk up to it, long-press is *not* needed — wait for the window, use the
   Provision tab as usual. (Or long-press if you can reach it.)

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Nothing in Scan tab | Tracker asleep between cycles (normal — beacon only during the awake window) or BT off / permissions missing. Scan over ≥ 2 cycles or raise the sleep frequency temporarily. |
| Config write fails | Device not in config window (long-press ≥ 3 s), wrong target MAC, or out of range. The CRC path rejects corrupted writes — retry; a *rejected* write changes nothing. |
| Remote tab empty | TTN not configured, wrong region, or API key without downlink/uplink-read rights. Check device ID. |
| No GPS fix ever | Indoor (the firmware backs GPS off to every 8th cycle automatically — wait for an outdoor cycle, or use **Locate now** outdoors). |
| `joined=false` in status | Device lost power (session lives in RTC RAM) — it rejoins on the next cycle; also verify keys. |
| Map empty | All history entries have `0,0` (never had a fix) — the app skips zero coordinates. |
