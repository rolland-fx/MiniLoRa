# Android App — User Guide

App version 3.0 (Jetpack Compose, Material 3). Requires Android 13+
(minSdk 33). The APK is built from `SW/Current/Arduino/Android-MiniLoRa`
(`./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`).

> Bringing up a **blank device** for the first time? Start with
> [HowToStart.md](../../../HowToStart.md) — this guide covers every tab in
> depth. The app also ships an in-app **Help & guide** ("?" icon, top bar)
> that always matches the installed version.

## Permissions (first launch)

The app asks for **BLUETOOTH_SCAN**, **BLUETOOTH_CONNECT** and
**ACCESS_FINE_LOCATION**. Grant all three — scanning and the config service
need them. It will also ask to enable Bluetooth; accept.

> BLE scanning is declared `neverForLocation`: turning the phone's
> **location services off** does **not** stop beacon reception (the app
> never derives position from scans — the devices carry GNSS).

## The four tabs

### 1 · Map (home)

Everything about the *data*, not the radio.

- **Status header** — the newest payload: temperature, humidity, coordinates
  (or `GPS: no fix`), battery voltage, satellite count, a source chip
  (`BLE`, `TTN`, `WiFi`, …) telling you which channel delivered it, and how
  **old** it is (`just now`, `4 min ago`, …). If the device has been
  configured over BLE at least once, a second line shows its live status:
  `fw 3.0.0 · bat 4021 mV · joined=true · gpsFails=0` — including warnings
  like `[LOW BAT]` or `[RECOVERY]`.
- **Per-device color tracks** — one color per identity (the remembered
  tracker is blue; peers follow: green, purple, orange…), drawn as a
  polyline with fading dots. Filter chips above the map select a device or
  group — each chip carries its track color as a legend dot.
- **Trek peer contact card** (trek mode) — the peer's latest relayed
  telemetry (sats, battery, temperature), who relayed it, and an
  **IN TOUCH / CONTACT LOST** state (lost = 3 missed exchange windows,
  mirroring the device's LED signal).
- **History** — swipeable strip of recent payloads (newest right). Each card
  shows the device name, temp/hum/pressure, time, source and coordinates.

*Data sources:* the **Devices** tab (BLE beacon reception) and the **Remote**
tab (TTN/WiFi polling) both feed this screen — and every TTN/WiFi pull also
**backfills recent frames** from the network, so the history covers cycles
that arrived while the app was closed. Frames from different feeds that
belong to the same device wake merge into ONE card showing all sources.
History is persisted on the phone and survives restarts.

### 2 · Devices

The BLE scan. It runs whenever the app is open (battery-balanced) — every
MiniLoRa advertisement in range is parsed and listed with **signal quality**
(excellent / good / weak) and last-seen. Tap a device to make it the target
for Setup and status reads (a snackbar confirms, with **Undo**). A trek
device shows a `relaying peer XXXXXXXX` badge when it is relaying its
buddy's position. Devices not heard for 60 s drop off the list.

> The beacon is a telemetry transmitter, not a config advertisement — you do
> *not* need to "connect" to it. Provisioning (Setup tab) is a separate GATT
> service the device opens when you long-press its button. The config
> advertisement is listed too, so the device is visible exactly when you
> need it.

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

### 4 · Setup

The device editor, organized as **sectioned cards** — radio mode first (it
decides which fields exist), then credentials, sensors, power. Requires a
**selected device** (Devices tab) and the tracker sitting in its **BLE
config window** — hold the button ≥ 3 s (the device wakes with a 120 s
window). Then:

- **Read from device** — chains a status read (battery, counters, joined)
  and a config read; the editor fields are populated from the device.
- **Write to device** — builds the body from the fields and sends it inside
  the ICD CRC-protected envelope (schema 4 = 168 B, schema 5 with P2P =
  193 B). A corrupted write is rejected by the device, never half-applied.
- **Export .cfg** — saves the editor state as the device's native envelope
  file (via the Android file picker, e.g. `minilora-<name>.cfg`).
  Read first to mirror the device; the file contains the TTN keys and WiFi
  password in clear text — treat it like a credential.
- **Import .cfg** — pick a `.cfg` file (generated by the app itself or the
  server's **Config builder** page); it is parsed and CRC-validated, the
  editor fills with its values — review, then **Write to device**.
  Nothing is written until you press Write. This is the device-cloning
  path: Read A → Export → Import on B → Write.
- Everything is kept in encrypted prefs (survives restarts).

**The cards:**

- **Target device** — shows the current selection (set on the Devices tab).
- **Radio mode** — a three-way choice with a one-line explainer each:
  - **LoRaWAN** (default) — via TTN; shows the JoinEUI/DevEUI/AppKey/NwkKey
    credential card.
  - **P2P star** — no cloud: this device TXes raw LoRa to a powered gateway
    which forwards to your LAN server. Shows the **gateway role** switch,
    the air profile (Long-range SF12 / Balanced SF10 / Bench SF7), the
    command-window switch, and the **mesh key** (32 hex — the private
    network's shared secret).
  - **Trek** — no cloud at all: two devices exchange positions every wake;
    both phones show both tracks. Same air-profile + mesh-key fields.
- **Wi-Fi & server** — SSID/password and the MiniLoRa-server IP+port (also
  used for OTA), in every radio mode.
- **Sensors & transports** — a switch grid (GNSS, BME280, OPT3001, LIS3DH,
  BLE beacon, Zigbee, LoRaWAN uplink, Wi-Fi push).
- **Power & behavior** — the wake-interval slider (1 min … 120 min) and the
  **Asset mode (P5)** block:
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
  configuration travels separately in the Setup tab (.cfg).

> The settings file contains the **TTN API key in clear text** — treat it
> like a credential.

## Typical workflows

**Provision a fresh tracker**
1. Flash it (see [FLASHING.md](FLASHING.md)), hold the button ≥ 3 s.
2. Devices tab → select it. Setup tab → **Read from device**, fill
   JoinEUI/DevEUI/keys from your TTN console → **Write to device**.
3. Done — it rejoins and reports on the next cycle. (TTN → device → check
   "Latest uplink".)

**Provision a trek pair**
1. Both devices: Setup tab → Radio mode **Trek**, the **same mesh key**,
   the same air profile and the same wake interval.
2. Power both outdoors — first contact lands within 1–3 windows; each map
   then draws its own track (blue) and the peer's (green), with the
   contact card showing IN TOUCH.

**Mount it and forget it**
1. Enable Zigbee in Setup only if you run a coordinator (optional).
2. Remote tab → enable TTN polling (phone) and note the diag cadence
   (~every 16 boots).
3. Later: Remote → **Locate now** to find it, **Send diag** to check its
   health from the couch.

**Reconfigure without touching it** (mounted high, waterproof box)
1. Remote → **Open BLE window** → within its next wake cycle the device
   advertises the config service for 120 s.
2. Walk up to it, long-press is *not* needed — wait for the window, use the
   Setup tab as usual. (Or long-press if you can reach it.)

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Nothing on Devices tab | Tracker asleep between cycles (normal — beacon only during the awake window) or BT off / permissions missing. Wait over ≥ 2 cycles or raise the wake frequency temporarily. |
| Config write fails | Device not in config window (long-press ≥ 3 s), wrong target MAC, or out of range. The CRC path rejects corrupted writes — retry; a *rejected* write changes nothing. |
| Remote tab empty | TTN not configured, wrong region, or API key without downlink/uplink-read rights. Check device ID. |
| No GPS fix ever | Indoor (the firmware backs GPS off automatically — wait for an outdoor cycle, or use **Locate now** outdoors). |
| `joined=false` in status | Device lost power (session lives in RTC RAM) — it rejoins on the next cycle; also verify keys. |
| Map empty | All history entries have `0,0` (never had a fix) — the app skips zero coordinates. |
| Trek peer shows CONTACT LOST | 3 missed windows (out of range, or the peer's battery died). The device's green LED triple-double-blinks too; any received frame clears it. |
