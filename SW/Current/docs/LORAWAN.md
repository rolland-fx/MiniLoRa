# LoRaWAN — Uplinks & Downlinks

Region **EU868**, **OTAA**, RadioLib LoRaWAN stack, ADR enabled. The full
byte-level contract is [`../ICD.md`](../ICD.md); this page is the operator's
view. Golden vectors: `SW/Current/Firmware/host_tests/vectors/`.

## Setup on The Things Stack

1. Create an **OTAA** device: copy **JoinEUI**, **DevEUI**, **AppKey**,
   **NwkKey** (LoRaWAN 1.0.4 style — both keys).
2. Provision them into the tracker over BLE (Config tab). The device ID in
   TTN is independent; the app falls back to the BLE device name if unset.
3. Frequency plan **EU_863_870**, region `eu1` (default in the app).
4. Register the payload formatter: copy `SW/Current/TTN/decoder.js` into the
   device's **Payload formatters → Javascript** (it switches on fPort, so
   telemetry and diagnostics both decode).

### Session behaviour

- The OTAA session (nonces + session keys) lives in **RTC RAM** — a device
  that stays powered sleeps and wakes **without rejoining**; a device that
  loses power rejoins once (same tradeoff as v1).
- **ADR is on**: as the tracker settles near gateways, the network lowers
  datarate/TX power — shorter, cheaper uplinks.
- **Class A** only: downlinks arrive in RX1/RX2 after an uplink. To reach a
  sleeping tracker, schedule a downlink for *after its next uplink* — the TTN
  console's "Downlink switch" and the app's Remote tab do exactly that.

## Uplinks

### fPort 1 — Telemetry, 28 bytes (frozen)

Sent **every cycle**. Layout (v1 contract, never change a byte):

```
 0 temp int8°C   1 hum u8%    2-3 pressure u16 LE hPa    4 battery 0-255
 5-6 lux u16 LE  7-9 acc x/y/z u8 (±10 g map)            10-13 lat int32 LE µ°
14-17 lon int32 LE µ°   18-19 alt int16 LE (m+1000)     20 sats u8
21-24 time u32 LE unix   25 awake_s u8   26 sleep_min u8   27 gpio bits
```

- **No-fix cycles send zeros** — detect with `lat==0 && lon==0 && sat==0`
  (the decoder.js does this for you). The explicit `gpsValid` bit rides the
  diag frame (§2 of the ICD).
- `time` is GPS time; if GPS gave no time the firmware estimates it from the
  **DeviceTimeReq** MAC answer (± a few seconds, never accumulating).

### fPort 2 — Diagnostics, 24 bytes, schema `0x03`

Sent every **16 boots** (~1/day at 15 min–2 h intervals) or on demand
(`SEND_DIAG` / the app's *Send diag*). Schema byte first, so decoders never
key on the firmware version:

```
 0 schema=0x03   1-3 fw version      4-5 bootCount     6-7 unexpectedResets
 8 resetReason   9-20 failure counters u16 LE (gps, join, send, wifi, zigbee, sensor)
21 gpsConsecFails  22 powerFlags (bit0 degraded, bit1 critical)
23 flags2 (bit0 gpsValid, bit1 OTA pending, bit2 recovery)
```

Interpretation tips: rising `gpsTimeouts` + `gpsConsecFails` = indoor/stored;
`loraJoinFailures` climbing with healthy battery = RF problem; `bootCount`
jumping by hundreds per day with `unexpectedResets` growing = a crash loop
(recovery mode would also be flagged).

### fPort 4 — Position history (store-and-forward, ICD 3.2)

On GPS-less cycles (throttled to every 4th) the device sends its **two most
recent fixes** (lat/lon/GPS-time/sats, oldest first) so underground periods
still produce positions. The server files each entry as a normal reading
tagged `ttn-hist`. Format: ICD §2.1.

### MAC-layer extras

- **LinkCheck** and **DeviceTime** requests are supported by RadioLib; the
  firmware uses DeviceTime for the time fallback. ADR/LinkADR is handled by
  the stack.
- The **diagnostics uplink also opens an RX window** — commands may arrive
  after it (the app and `downlink.js` exploit this).

## Downlinks

Two ABIs are active:

### fPort 3 — Remote commands, 3 bytes

```
[0x4D][opcode][arg]
```

| Opcode | Name | Effect (next wake cycle) |
|---|---|---|
| `0x01` | LOCATE_NOW | forced GPS window (overrides backoff/low-battery skip; never critical battery) + report again after 60 s |
| `0x02` | SEND_DIAG | diagnostics frame on the next cycle |
| `0x03` | RESET_DIAG | clear all failure counters + boot-loop guard |
| `0x04` | BLE_WINDOW | BLE config service open for 120 s next cycle |
| `0x05` | OTA_UPDATE | pull the active image from the MiniLoRa-server next wake (see the server's /ota page) |
| `0x06` | CFG_SET | set a transport/sensor enable (ICD 3.5 §3): arg bit7 = value, bits0–3 = flag id — 1 WiFi, 2 Zigbee, 3 BLE beacon, 4 GNSS, 5 BME280, 6 OPT3001, 7 LIS3DH; saved to NVS, confirmed on the next diag frame's `config_flags` byte. **LoRaWAN itself is not addressable.** |
| `0x07` | SET_RANGE | set the sleep duration (ICD 3.6 §3): arg = the sleep-table index 0–8 (1/2/5/10/15/20/30/60/120 min — the config tab's scale); saved to NVS, confirmed on the next telemetry frame's sleep-minutes byte |

Unknown opcodes are ignored (forward-compatible). **No secrets are accepted
over downlink** — key provisioning is BLE-only by design. The TTN downlink
queue can be cleared from the app's Remote tab or the server's device
Remote page (it cancels pending commands from any surface).

**From the TTN console:** Messaging → Downlink → FPort `3`, bytes `4D 01 00`,
*replace* (not push) if you want it to survive a few cycles.
**From the app:** Remote tab → command buttons.
**Programmatically:** `SW/Current/TTN/downlink.js` (`encodeDownlink` hook) or
the TTN v3 `downlink/push` REST call the app uses.

Sleep duration is changed via the BLE config envelope or the **SET_RANGE**
command (fPort 3, ICD §3); fPort-1 downlinks are ignored.

## Testing without hardware

- `SW/Current/TTN/check_vectors.py` (or `.js`) validates the decoder against
  the golden vectors, including the diag frames and command bytes.
- On the bench: set `MINILORA_BAT_LOW_MV` absurdly high to force degraded
  mode, or disable the antenna to watch the indoor backoff — the serial log
  narrates every decision.

## Expected airtime & energy (EU868, SF7 default)

A 28-byte uplink at SF7 is ~50 ms on-air; ADR may push it higher in bad
RF. The whole radio session (join-or-restore + uplink + RX windows) is
budgeted ~3–5 s per cycle — see the energy budget in
`Firmware/docs/POWER_OPTIMIZATION.md`.
