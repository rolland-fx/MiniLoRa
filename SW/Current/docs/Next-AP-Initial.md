# Next: AP Initial Config — SoftAP Captive-Portal Provisioning

**Status:** planned (approved concept) · **Date:** 2026-11-04

The scenario: a device **arrives at a client's place as blank as possible** and
the client sets it up themselves — no app, no laptop tools, any phone (iOS
included), no internet needed during setup. The device hosts its own config
portal over WiFi; the client fills the same field set the BLE config write
accepts today, and the device reboots into its normal cycle.

---

## 1. Feasibility — measured, not guessed

| Resource | Budget | Portal cost | Verdict |
|---|---|---|---|
| **Flash (per OTA slot)** | `ota_0`/`ota_1` = 0x1E0000 = 1,966,080 B each (fw 3.0 layout); current bin (3.0.0) ≈ 1,866,000 B → **~100 KB free per slot** | **+12–18 KB** total (see breakdown below) | ✅ ~70 KB margin |
| **RAM (D/IRAM)** | ~226 KB free of 452 KB (idf_size, fw 3.0 map) | SoftAP ~25–30 KB + NimBLE (already resident in config mode) ~35 KB + minimal HTTP ~1 KB | ✅ both stacks run concurrently |

Flash cost breakdown (the design keeps it small on purpose):

| Piece | Cost |
|---|---|
| Hand-rolled minimal HTTP responder (lwIP sockets, ~200 lines) | ~3–5 KB code |
| Embedded page, **pre-gzipped**, served with `Content-Encoding: gzip` | ~4–6 KB rodata (vs ~15 KB raw) |
| SoftAP start/stop glue + LED + timeout wiring | ~1–2 KB |
| Tiny DNS responder (wildcard A record → 192.168.4.1) | ~1 KB |

Two deliberate decisions:

1. **Do NOT link `esp_http_server`** — it is currently *not* in the binary
   (verified: no `httpd_*` symbols in `build/minilora.map`) and would cost
   ~25–30 KB. A captive portal needs exactly two routes (`GET /`,
   `POST /apply`) plus a few OS captive-probe answers (`/generate_204`,
   `/hotspot-detect.html`, `/connecttest.txt`) — a raw-TCP responder covers
   that in a few KB.
2. The page ships **gzip'd in rodata**; every captive-portal browser accepts
   gzip.

**Escape hatch** (only if a future feature eats the margin): the partition
table is at exactly 4 MB, but `zb_storage` and `coredump` are 128 KB each —
shrinking either regains 64 KB per OTA slot.

Partition table for reference (4 MB flash, ESP32-C6):

```
nvs        0x9000   0x6000
phy_init   0xf000   0x1000
otadata   0x10000   0x2000
ota_0     0x20000  0x1E0000   (fw 3.0 layout)
ota_1     0x200000 0x1E0000
zb_storage 0x3E0000 0x10000
coredump   0x3F0000 0x10000
```

---

## 2. The client's unboxing flow

1. Client peels the battery tab → device boots, sees **blank config** →
   auto-enters setup mode: LED pulse + open AP **"MiniLoRa-Setup"** + the
   existing BLE config service, simultaneously
2. Phone joins the AP → captive portal auto-pops (or the client visits
   `192.168.4.1`)
3. Form: name, WiFi SSID/password, server IP:port → **Submit**
   (LoRaWAN/behavior/asset sections collapsed as "advanced")
4. LED confirms, device reboots into its normal cycle — reporting to the
   configured server

No app. No URL typing beyond the auto-popup. No internet needed during
setup. Works on any phone.

---

## 3. Firmware architecture

- **A small `ConfigMode` module** owns the window: "portal/BLE open, one-shot
  apply path, LED, timeout". The existing entry paths (long-press, recovery
  mode, downlink 0x04 BLE window) keep working unchanged — config mode just
  becomes **transport-agnostic**: BLE config service *and* SoftAP portal in
  the same window, whoever writes first wins.
- **Reuse is the whole trick.** The HTTP handler receives the raw schema-4
  write envelope (166 B: `'C'`, schema 4, 164-byte body, CRC16-CCITT) — the
  **same envelope the Android app writes** — and hands it to the existing
  `parseConfigWrite()` (`main/domain/Config.hpp`), which already validates
  magic/version/CRC and leaves the config untouched on corruption. One
  parser, two transports; the existing host tests for envelope corruption
  carry over.
- **Blank detection**: an explicit "NVS had no config" flag on load (do NOT
  infer it from the defaulted struct — `makeDefaults()` fills build-time
  fallbacks). Blank → setup mode instead of the normal cycle. Configured
  devices keep today's behavior exactly.
- **Apply = `ConfigStore::save()` + LED confirm + `esp_restart()`** — a clean
  boot into the first real cycle, and the natural resolution of BLE-vs-HTTP
  write races (first valid write wins, restart).
- **Timeouts/battery**: setup mode (blank device) runs 10 min → sleeps 60 s →
  re-offers, looping until configured, with the battery-critical guard.
  Configured devices keep the current 120 s window on long-press.

---

## 4. The portal page (full config parity — the same field set as BLE)

The form mirrors `DeviceConfig` (`main/domain/Config.hpp`) exactly — the
device writes the same wire format the app does (ICD §4):

**Basics (visible)**

| Field | Config entry |
|---|---|
| Device name | `globalName[32]` |
| WiFi SSID | `wifiSSID[32]` |
| WiFi password | `wifiPassword[32]` |
| Server IP | `wifiServerIP` (u32, dotted quad) |
| Server port | `wifiServerPort` (u16) |

**LoRaWAN (collapsed "advanced")** — the client copies these from the sheet
shipped with the device (hex inputs):

| Field | Config entry |
|---|---|
| JoinEUI | `joinEUI` (u64) |
| DevEUI | `devEUI` (u64) |
| AppKey | `appKey[16]` |
| NwkKey | `nwkKey[16]` |

**Behavior (collapsed)**: sleep-range dropdown (`cfgRange` → `kSleepTableMs`
labels) + the transport/sensor checkboxes `cfgLoRaWAN`, `cfgWiFi`, `cfgGNSS`,
`cfgBME280`, `cfgOPT30`, `cfgLIS3DH`, `cfgBLE`, `cfgZigbee`.

**Asset mode (collapsed, schema-4 fields)**: `cfgAssetMode`, `cfgMotionSens`
(1=128 mg / 2=256 mg / 3=512 mg), `cfgAssetHeartbeatH`, `cfgBurstIntervalMin`,
`cfgBurstMaxPerDay`.

Page behavior: JS builds the schema-4 envelope locally (CRC16-CCITT,
poly 0x1021, init 0xFFFF — same as `crc16Ccitt()` and `gen_vectors.py`) and
`fetch('/apply', {method:'POST', body: envelopeArrayBuffer})` — no server
round-trip, works fully offline off the device's own AP. The handler replies
200/400 and the page shows the saved/rejected state (a bad CRC never
misconfigures keys — same guarantee as BLE).

---

## 5. Known limitations (honest list)

- **Plain HTTP over an open AP** — the WiFi password is sniffable on-site
  during the window. This is *exactly* the exposure the BLE config path has
  today (plain GATT write). Accept for v1; a printed-PIN AP password is a
  trivial hardening later.
- **No hostnames** — the config ABI stores the server as IP + port
  (`wifiServerIP` u32). The client must be given the server's IP (or a fixed
  address). A device-side DNS resolver would be a separate feature.
- **Wrong WiFi = silent failure** — the device can't tell the client. They
  long-press to re-enter setup. (A "wrong password?" retry pattern is
  possible later; not v1.)
- **Blank device in transit** — an attacker could push a config to a blank
  device if it powers on in transit. Mitigation: ship with the battery
  disconnected / tab in place (the device only powers on at the client).

---

## 6. Bench test path (before it ships to a client)

1. Portal form → envelope → **verify bytes identical** to what the app's
   config builder produces for the same fields (golden vectors via
   `gen_vectors.py` CRC + existing host tests — free regression coverage)
2. Blank-boot auto-portal (erased NVS → setup mode, no LoRa TX)
3. Timeout → sleep 60 s → re-offer loop; battery-critical guard
4. Long-press re-entry on a configured device (BLE + AP both offered)
5. Corrupted POST (`/apply`) rejected, old config kept, LED signals reject
6. BLE-write and HTTP-write race (first write wins, restart)
7. End-to-end client-style test: phone-only setup against the LAN server —
   form → save → reboot → first readings visible on the server web UI

---

## 7. Effort estimate

| Task | Time |
|---|---|
| `ConfigMode` refactor + SoftAP + DNS + raw HTTP handler | ~1.5 d |
| Portal page + JS envelope builder (+ gzip embed) | ~1 d |
| Blank-boot semantics, LED/timeout wiring, battery guard | ~0.5 d |
| Bench pass + golden-vector check | ~0.5–1 d |
| **Total** | **~3–4 days** |

Ships as a firmware release, deployed via the usual OTA.

---

## 8. Context: why this option (decision record)

Other paths considered for "initial config without the app":

- **Web Bluetooth page** (browser writes the envelope over BLE) — zero
  firmware change but Android/desktop-only (no iOS), and it needs a hosted
  HTTPS page. Better as a *vendor support tool* than a client flow. Kept as
  a future stopgap.
- **Serial/USB config** — trivial but bench-only; useful later as the
  key-burning step for a fleet.
- **Bootstrap-by-join** (keys pre-burned, full config arrives chunked over
  encrypted LoRaWAN downlink after first join) — the true zero-touch north
  star for a fleet (~1 week: config-receive state machine, chunked
  reassembly, per-device config store on the server; needs an ICD 3.6
  exception to "no secrets over downlink" for session-encrypted frames).
  Revisit when a second device exists. The SoftAP portal remains the
  fallback for sites without LoRa coverage.
- **NVS partition image via esptool** — factory tooling only; brittle to
  config-struct changes; only worth it at volume.

**Decision:** SoftAP captive portal (this document) — any phone, no
infrastructure, and it composes with the existing config-mode design instead
of adding a parallel one.
