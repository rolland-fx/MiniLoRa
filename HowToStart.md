# HowToStart — bring up a blank MiniLoRa tracker

From a **fully blank device out of the box** (no config in flash) to
readings on your server, in about 15 minutes. No soldering, no USB needed
after the first firmware flash.

> **What "blank" means:** erased NVS — no WiFi credentials, no LoRaWAN keys,
> no name. The tracker cannot report anywhere until you provision it.
> If your device runs firmware **2.6.13 or older**, update it first (see
> [FLASHING.md](SW/Current/docs/FLASHING.md)).

---

## 1 · What you need

| Thing | Why |
|---|---|
| The tracker (battery disconnected or switch off) | power comes last |
| **Android phone** with the MiniLoRa app (see [USER_GUIDE.md](SW/Current/docs/USER_GUIDE.md)) | provisioning runs over BLE; **Android 13+** |
| Your **TTN console** (or LoRaWAN network server) with a registered device | you will copy the JoinEUI / DevEUI / AppKey / NwkKey from here |
| The **MiniLoRa-server** reachable from the tracker's WiFi (LAN IP, e.g. `192.168.1.100:5000`) | where the readings land; TTN also forwards if you prefer |
| WiFi 2.4 GHz credentials | the tracker is a 2.4 GHz-only station |

> **iOS?** The config editor is Android-only today. A captive-portal setup
> (no app at all) is planned — see
> [Next-AP-Initial.md](SW/Current/docs/Next-AP-Initial.md).

## 2 · Register the device in TTN (once per tracker)

1. TTN Console → Applications → **your app** → *+ Register device*.
2. Choose the **915/868 profile** for your region; generate or enter the keys.
3. Note four values — you will type them into the app in step 4:
   **JoinEUI, DevEUI, AppKey, NwkKey** (the device uses OTAA).

Keep them secret: the keys *are* the device's identity.

## 3 · Power on and open the config window

1. Connect the battery. The tracker boots with **no config** — it will not
   transmit anything meaningful yet, which is expected.
2. **Hold the user button ≥ 3 s** — the BLE config service opens for
   **120 s** (an LED marks the window; release the button once it lights).
3. If you miss the window: press again. It never transmits sensitive data
   outside a window you opened.

## 4 · Provision from the app

1. **Scan tab** → wait for the tracker's beacon → **tap it** to select it as
   the target.
2. **Provision tab** → fill in:
   - **Device name** — what the server keys your data by (e.g. `MiniLoRa-FX-1`)
   - **WiFi SSID / password** — 2.4 GHz network the tracker can reach
   - **Server IP + port** — your MiniLoRa-server (LAN IP, default port 5000)
   - **JoinEUI / DevEUI / AppKey / NwkKey** — from step 2
   - **Sleep duration** — 15 min is a good start; shorter cycles cost battery
   - Transports/sensors — leave everything ON except **Zigbee** unless you
     run a coordinator
3. **Write to device.** The write is CRC-protected: a corrupted write is
   rejected by the device, never half-applied. The tracker saves it to NVS
   and starts its normal cycle immediately.

> **No secrets over the air:** keys only ever travel over BLE, inside the
> CRC-protected envelope — never via LoRaWAN downlink.

## 5 · Verify

1. **App → Remote tab**: after the tracker's next wake, the *Diagnostics*
   card fills in (fw version, boot count, `joined`, config flags) and the
   *Downlink queue* card tracks any commands you send.
2. **TTN console** → device → *Latest uplink*: fPort-1 telemetry arriving.
3. **Server** → `http://<server>:5000/device/<name>`: the hero cards,
   charts and map populate — including both identities if the tracker
   reports over WiFi **and** TTN (grouped in one row on the dashboard).

A GPS fix takes seconds when the sky is visible and up to a few minutes
indoors (the firmware backs GPS off automatically when it can't see the sky;
**Locate now** in the Remote tab forces an immediate window).

## 6 · Everyday use

- **Sleep duration, transports, sensors** — Remote tab (app) or the server's
  *Remote* page; changes persist on the device and confirm on the next diag.
- **Re-provision later** — long-press again (or *Open BLE window* from the
  Remote tab if it is mounted out of reach).
- **Firmware updates** — over the air: upload the image on the server's
  *OTA* page, activate, notify; the tracker pulls it on a wake (see
  [FLASHING.md](SW/Current/docs/FLASHING.md)).

## Troubleshooting

| Symptom | Fix |
|---|---|
| Provision tab finds nothing | Not in config window (long-press ≥ 3 s), wrong target in the Scan tab, or Bluetooth permission missing |
| Write rejected | Out of range or window closed — retry; a rejected write changes nothing |
| No TTN traffic | Keys/JoinEUI mistyped (most common), or the device is indoors with a bad antenna angle — check the diag frame's `lora_join_failures` |
| Server sees nothing | Wrong server IP/port, tracker and server on different subnets, or WiFi credentials wrong — the diag's `wifi_failures` counter tells you |
| Everything looks right but no data | Check **Remote config** in the app: WiFi and LoRaWAN must be ON (they are remote-togglable) |

## Deep details

- Wire contract (every frame): [SW/Current/ICD.md](SW/Current/ICD.md)
- Full manual: [SW/Current/docs/README.md](SW/Current/docs/README.md)
- Remote commands: [SW/Current/docs/LORAWAN.md](SW/Current/docs/LORAWAN.md)
- The planned app-free captive-portal setup:
  [SW/Current/docs/Next-AP-Initial.md](SW/Current/docs/Next-AP-Initial.md)

## Screenshots (Android app)

> **TODO — screenshots to add** to `docs/screenshots/` (see the root
> README table): `app-tracker.png`, `app-graphs.png`, `app-remote.png`,
> `app-provision.png`.
