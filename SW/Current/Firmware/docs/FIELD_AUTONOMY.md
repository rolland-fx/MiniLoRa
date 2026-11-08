# Field Autonomy v2.2.0 — Remote Commands, Network Time, Awake Trim

**Date:** 2026-10-31 · **FW version:** 2.2.0
**Goal:** close the two remaining gaps of a *deployed* tracker (can't reach it
physically; can't trust timestamps without GPS) and shave the residual awake
time (P2).

## 1. Remote command set (downlink fport 3)

### ABI

Downlink frame: **`[0x4D][opcode][arg]`** — magic `'M'` avoids colliding with
the v1 fport-1 ABI (byte 0 = sleepIdx, 0–7). The command frame is accepted
after **any** uplink (fport 1 or 2); v1 sleepIdx/gpio6 semantics still parse
only on fport 1. LoRaWAN provides integrity; **no secrets are ever accepted
over downlink** — key material only via BLE config.

| Opcode | Name | Effect (applied at the START of the next cycle) |
|---|---|---|
| `0x01` | `LOCATE_NOW` | Force a GPS window this-next cycle (overrides indoor backoff + low-battery GPS skip, never critical battery) and sleep only **60 s** after it. |
| `0x02` | `SEND_DIAG` | Diagnostics uplink (fport 2) on the next cycle. |
| `0x03` | `RESET_DIAG` | Clear all failure counters + boot-loop guard. `bootCount` is kept (diag cadence + backoff phase depend on it). |
| `0x04` | `BLE_WINDOW` | Run the BLE config service for the full 120 s window on the next cycle. |

Unknown opcodes are logged and ignored (forward-compatible). Commands ride
RTC flags across the deep sleep between the downlink cycle and the next one.

### TTN usage

Console → End devices → your device → **Downlink**, FPort **3**, payload e.g.
`4D 01 00` = locate now. Scheduling a downlink on fport 1 with the v1 ABI
(`sleepIdx gpio6`) continues to work unchanged.

## 2. Network-time fallback (DeviceTimeReq)

When a cycle has **no GPS time** (`epochSec == 0`), the uplink queues a
`DeviceTimeReq` MAC command (8 bytes FOpts; RadioLib
`sendMacCommandReq(RADIOLIB_LORAWAN_MAC_DEVICE_TIME)`). The answer is stored
in RTC RAM together with the sleep duration that followed, so the next
GPS-less cycle reports

```
epochSec ≈ netEpoch + lastSleepMs/1000 + awakeMs/1000
```

— accuracy a few seconds, **never accumulating**: every fallback cycle
re-requests DeviceTime, so the reference is at most one cycle old. Cycles
with a valid GPS time neither request nor use the fallback. TTN supports
DeviceTimeReq natively (LoRaWAN 1.0.4 MAC).

Value: positions/timestamps from the server's perspective are never
meaningless zeros; combined with `gpsValid = 0` the backend can always tell
*when* the device was alive.

## 3. Awake-time trim (P2, deferred from the P1/P3/P4 round)

| Item | v1 / v2.1 | v2.2 |
|---|---|---|
| Battery read | 200 ms settle + 8×100 ms ≈ 1.1 s | 100 ms settle + 8×20 ms ≈ **0.26 s** |
| BME280 samples | 4 | 2 (payload resolution is 1 °C / 1 %) |
| OPT3001 samples | 4 | 2 (payload resolution is 1 lux) |
| LIS3DH samples | 8 × 5 ms | 4 × 5 ms |

GPS wake delay (500 ms) deliberately **kept** — it protects the warm-start
TTFF, which dominates far more. Net saving ≈ **1–1.5 s per cycle** ≈
0.05–0.1 mAh/day at 15 min interval; small, but free and it shortens every
future cycle type (including locate-now).

## 4. Files

- `transports/lorawan/LoRaWanTransport.{hpp,cpp}` — command ABI, MAC
  request/answer, unified downlink parse (any fport).
- `app/App.cpp` — RTC command flags, BLE-window phase, locate-now override,
  net-time fallback + propagation, sensor sample counts.
- `app/Stores.{hpp,cpp}` — unchanged (reset handled in-memory so the
  sleep-phase save persists it; an NVS-only reset would be overwritten).
- `drivers/BoardPower.cpp` — battery read timing.

## 5. Validation checklist

- [x] `idf.py build` green (1.80 MB, 4 % headroom in the OTA slot)
- [x] Host tests 85/0
- [ ] TTN downlink `4D 01 00` on the bench → serial shows `locate now`,
      forced GPS window, next sleep 60 s
- [ ] `4D 03 00` after some failures → diag frame reports zeros
- [ ] Indoor (no antenna) → uplink without GPS time, next diag shows
      epochSec ≠ 0 sourced from the network
- [ ] Verify DeviceTimeAns actually returns on TTN (check `network time`
      in the serial log; some networks only answer on Class A if queued)
