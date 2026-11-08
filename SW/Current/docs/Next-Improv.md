# Next improvements — derived data & feature ideas

*Written 2026-11-05, from a "what else could we compute, given sensor / memory /
flash / CPU?" session. Nothing here is implemented yet except where marked —
the wake-on-motion section documents code that is **already shipped**.*

## Ground rules (where the real constraints are)

| Surface | Actually scarce | Effectively free |
|---|---|---|
| **Firmware (ESP32-C6)** | the 28-byte payload, complexity/bug risk | CPU + flash (extra math is microseconds against a radio burst of seconds); RAM a few RTC slots at a time |
| **Server** | nothing | everything — limited only by *what the frames carry* |
| **App** | nothing since the 400-entry backfill/persistence | phone sensors (currently unused) |

Rule of thumb: **never compute on-board what the server can derive** — it
duplicates logic across codebases and spends payload bytes for nothing.
Add a payload byte only when it unlocks something the server *cannot*
reconstruct (events, counters, quality flags).

## Already shipped: wake-on-motion (P5) — no work needed, just opt-in

Discovered while costing this idea: the feature is **fully implemented and
deployed** in the current firmware (2.6.16). It is gated behind the asset-mode
config flag, so it costs nothing until enabled per device.

| Piece | Where |
|---|---|
| Shared `PPS_INT` net (GPS PPS push-pull + LIS3DH INT1) handled by **open-drain INT1** (`CTRL5 D4D_INT1`) — pulls low only, coexists with PPS | `Lis3dh.cpp` init(); schematic note: both toggleable via I2C CMD |
| Wake path: GPIO4 RTC input + pull-up held, EXT1 `ANY_LOW` (bit4 = motion, bit7 = button) | `Sleep.cpp` `prepareSleepMotion()` |
| Sensor config: 10 Hz low-power (~4 µA), AOI1→INT1, thresholds **128/256/512 mg** (`cfgMotionSens`, default 256) | `Lis3dh::configureMotionWake()` |
| Wake decode: `g_motionWake` RTC flag + `earlyWake` guard for the net-time fallback | `App.cpp` `runCycle()` |
| What a motion wake does: **burst cycle** — forced GPS window + uplink, capped `cfgBurstMaxPerDay` (20/day), spaced `cfgBurstIntervalMin` (5 min), RTC-day gated; otherwise sleeps to `cfgAssetHeartbeatH` (24 h) | `App.cpp` asset-mode phase + `prepareSleepMotion(assetMs)` |
| Config exposure | ICD §4 schema-4 body bytes (`cfgAssetMode`, `cfgMotionSens`, `cfgAssetHeartbeatH`, `cfgBurstIntervalMin`, `cfgBurstMaxPerDay`) + server config-builder form |

**Power accounting** (LIS3DH 10 Hz LP ≈ 4 µA additive to ~7 µA ESP32 deep
sleep; active phase dominated by GPS TTFF + radio):

| Sleep regime | Extra cost of motion wake |
|---|---|
| 15 min cycle (current default) | **+0.1–0.2 %** — noise |
| 60 min | ~+5 % |
| 240 min heartbeat (asset) | ~+10–15 % — the price of "know it moved" vs a 4 h blind spot |

**Open items before trusting it in the field**

1. **LIS3DH wake→read timing bench check** — acc reads constant 133/133/133
   across cycles smells like reading too early after wake. In asset mode the
   sensor is the *wake source*, so this timing becomes load-bearing.
   (V1 used 25/50 ms settle delays.)
2. **Verify the burst path respects the critical-battery gate** — motion must
   not force a GPS cycle when the cell is at cutoff.
3. **PPS contention edge**: if the GPS module's PPS output is enabled *and*
   INT1 pulls low at the same instant, the push-pull driver briefly fights the
   open-drain. Harmless in practice (PPS pulses are ~100 ms), but the
   schematic hints the module's PPS can be disabled over I2C — worth a look
   if the GPS is configured to emit PPS.
4. **Field test**: enable asset mode (config builder → Provision/import),
   `cfgMotionSens = 2`, bench-shake → expect `wake=motion` log line + burst
   cycle; then compare battery slope on the analytics page (this also
   validates the µA math above against reality).

## Server-side candidates — zero firmware cost

All derivable from frames **already stored**. Ranked loosely by value/effort.

| Derived info | From | Value | Effort |
|---|---|---|---|
| **Delivery ratio** (sent vs stored, per gateway/day) | TTN `f_cnt` — **already in the storage-API frames**, zero payload cost | definitive "is my link OK"; validates the store-and-forward drip | S |
| **Battery projection** (days-to-low at current slope; what-if per sleep setting) | `bat_mv` slope + `sleep_min` + `awake_s` | turns the battery graph into a *warning* | S |
| **Missed-cycle detection** (expected wake = last_seen + sleep_min vs actual) | stored frames | distinguishes "sleeping" from "dead" | S |
| **Dew point, heat index, comfort band** | temp + hum | makes the BME280 pair actually useful | S |
| **Pressure trend + crude forecast** (Δ3 h/Δ6 h, rapid-drop storm flag, Zambretti-ish) | pressure history | the BME280's killer app | M |
| **Indoor/outdoor classification** | pressure-shape correlation vs the day's trend | context for every other metric | M |
| **Barometric altitude + vertical rate** (zero-point calibrated against GPS fixes; track floors without GPS) | pressure + GPS altitude | GPS-denied altitude | M |
| **Places / geofence dwell** (cluster fixes into known spots, time-per-place, "left home" event) | GPS history | turns the map into behavior | M |
| **Odometer, max speed, moving/stationary ratio** | consecutive fixes | distance exists; speed/ratio missing | S |
| **Motion index / transport classifier** (stationary / walking / driving from acc variance signature) | acc history | LIS3DH becomes an activity logger retroactively | M |
| **Energy audit** (radio-on time vs cycles, cost per transmitted byte) | `awake_s`, cycle gaps | explains battery-slope changes; measures the motion-wake delta for real | S |
| **GPS health trend** (TTFF vs sats vs time-of-day) | diag history | catches "antenna now under metal" weeks early | S |
| **Sensor cross-checks** (GPS alt vs barometric alt; implausible-value rates) | readings + diags | data-quality panel; also catches unit regressions like the acc mg fix | S |
| Seasonal baselines + per-hour-of-day norms, anomaly flags | long history | "unusually hot at 14:00" without thresholds | M |

## On-board candidates — only if it saves uplinks or creates events

| Idea | Cost | Payoff | Status |
|---|---|---|---|
| 1-byte payload sequence number | 1 payload byte (audit ICD §2 for a free byte first) | true loss detection without TTN `f_cnt`; validates the store-and-forward order | idea |
| RTC pressure slots (3×uint16) → weather-trend byte, or a delta-compressed history burst (fPort-4-style, reusing the position-history path) | a few RTC bytes + one fPort | offline device reports "temp min/max, pressure fell 2 hPa" in ONE frame | idea |
| Impact / free-fall counter, orientation bit (face up/down) | diag `flags2` spare bits | drop detection for shipped assets | idea |
| HDOP byte next to `sats` | 1 byte | fix *quality* — explains "7 sats but junk" | idea |
| **Not worth it**: duplicating server math on-board; any new sensor (air quality etc.) — that's hardware, not extrapolation | — | — | rejected |

## App-side candidates — the phone is an unused sensor

| Idea | Value |
|---|---|
| **Local notifications** from the poller: no-data-24 h, low battery, geofence exit, temperature extremes | the app has all data + a background loop; it just never *alerts* |
| Phone-barometer cross-check when BLE-near (floor-level altitude); phone-GPS vs device-GPS accuracy comparison during bench tests | sensor fusion for free |
| Home-screen widget (latest card) | glanceable tracker state |
| Range export/share (CSV/PDF) from a picked window | server already has CSV; the app could reuse its own history |

## Recommended order

1. **Delivery ratio via TTN `f_cnt`** — server-only, one query, definitive
   link-health answer.
2. **Battery projection + missed-cycle detection** — server-only, uses
   columns already stored; converts two graphs into two warnings.
3. **Asset-mode field test + the bench checks above** — validates the
   already-built wake-on-motion instead of building anything.

*Everything else: pick up when a use case actually asks for it — the list is
a menu, not a queue.*
