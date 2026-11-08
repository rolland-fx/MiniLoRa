# Golden Payload Vectors — Payload v1 Contract

**Status:** ✅ Phase 0 complete (tag: post-`Before-Rework`)
**Note:** the *canonical* wire-contract document is now `SW/Current/ICD.md`;
this file remains the payload-v1 golden-vector reference. Diag/config/downlink
vectors live in `host_tests/vectors/icd_v3.json` (same generator).

## Purpose

One set of **28-byte test vectors** shared by every implementation of the payload
contract, so encode/decode drift — the bug class that let the old `decoder.js`
silently rot — cannot happen again:

| Consumer | Fixture | Test |
|---|---|---|
| v2 firmware (host tests) | `host_tests/vectors/vectors.hpp` | Phase 2 unit tests |
| TTN `decoder.js` | `host_tests/vectors/payload_v1.json` | manual/CI check |
| Android app (`MiniLoRaPayload.kt`) | `host_tests/vectors/payload_v1.json` | manual/CI check |
| Flask test server (`server.py`) | `host_tests/vectors/payload_v1.json` | manual/CI check |

## Canonical rules

The **generator is the source of truth**:
`SW/Current/Firmware/tools/gen_vectors.py` (pure Python, stdlib only).

```bash
python3 tools/gen_vectors.py   # regenerates vectors.hpp + payload_v1.json
```

Encoding (identical to v1 firmware `buildUplinkPayload` at tag `Before-Rework`):

| Byte | Field | Rule |
|---|---|---|
| 0 | Temp | int8, °C direct |
| 1 | Hum | uint8, %RH |
| 2–3 | Pressure | uint16 LE, hPa |
| 4 | Battery | `((mv − 2500) × 255) // 1800` (v1 truncating map) |
| 5–6 | Luminosity | uint16 LE, lux |
| 7–9 | Acc X/Y/Z | `((raw + 10000) × 255) // 20000` |
| 10–13 | Latitude | int32 LE, microdegrees |
| 14–17 | Longitude | int32 LE, microdegrees |
| 18–19 | Altitude | int16 LE, meters + 1000 |
| 20 | Satellites | uint8 |
| 21–24 | Time | uint32 LE, unix epoch s |
| 25 | AwakeSec | uint8 |
| 26 | SleepMinutes | uint8 (index into `SLEEP_T`, 0–8 ⇒ 1–120 min) |
| 27 | GPIO | bit0 = GPIO6/LED_GREEN, bit1 = Button_2 |

**Known v1 inconsistency, reconciled in Phase 0:** the worked example in
`payload_format.md` had a hex string whose decoded lat/lon/time did not match
its own value table (hand-computed). The generator now emits a *consistent*
example; the doc's hex and table were updated to match. The **format itself is
unchanged** — decoder, app and server are unaffected.

**v2 divergence (intentional):** vectors flagged `"saturating": true` exercise
out-of-range accelerometer / battery inputs. v2 **clamps** to 0/255; v1 wrapped
(modulo-256 truncation — garbage values). In-range vectors are byte-identical
between v1 and v2. See `PHASE2_DOMAIN.md`.

## Vector set

| Name | Covers |
|---|---|
| `doc_example` | The payload_format.md worked example (reconciled) |
| `negative_coords_night` | Negative lat/lon/alt, sub-zero temp, accel negatives |
| `no_fix` | GPS enabled but no fix: zeros + sat=0 |
| `edges` | In-range extremes of every field (bat 4300 mV, lux 65535, ±10000 counts) |
| `deep_winter_long_sleep` | Max sleep index (120 min), GPIO bits set |
| `acc_saturation` | Saturating (v1/v2 divergence documented) |
