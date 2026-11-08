# Tracker Power Optimization (P1/P3/P4)

**Date:** 2026-10-31 · **FW version:** 2.1.0 · **Goal:** maximize battery life
for the tracker use case (long sleep intervals, low awake rate).

## 1. Energy budget (measured + datasheet numbers)

| Consumer | Value | Source |
|---|---|---|
| GPS acquisition | 36 mA | PA1010D datasheet |
| GPS **backup mode** (sleep) | **15 µA** | PA1010D datasheet — v1/v2 already use it correctly; ephemeris + time retained → hot/warm starts |
| ESP32-C6 deep sleep | ~7 µA | datasheet |
| Board sleep total (est.) | ~25–40 µA | GPS 15 + ESP 7 + buck + leakage — **verify with Joulescope** |
| Rest of awake cycle | ~25 mA avg | CPU + radios idle |

**≈95 % of the energy is the awake window, and the GPS dominates it.**
Sleep current is already near-optimal — the lever is *awake time*, not sleep
current.

Per-cycle estimate (15 min interval, outdoor): GPS warm fix 5–15 s ≈
0.05–0.15 mAh + overhead ~25 s ≈ 0.17 mAh + TX/RX ≈ 0.03 mAh
→ **0.3–0.4 mAh/cycle**, ~0.6–1 mAh/day sleep.

| Interval | Total/day | 1100 mAh battery | + solar assist |
|---|---|---|---|
| 15 min | ~25 mAh (v2.1) | **~5–6 weeks** | energy-neutral with ~1.5–2 h sun/day |
| 1 h | ~10 mAh | **~4–5 months** | energy-neutral with ~1 h sun/day |
| 2 h | ~6 mAh | **~6–8 months** | energy-neutral with < 1 h sun/day |

(Runtimes use 80 % usable capacity of the 1100 mAh cell; deep-sleep current
still to be confirmed by measurement, see §4.)

## 1.1 Power source (hardware)

| Component | Detail |
|---|---|
| **Battery** | 1-cell Li-ion, **1100 mAh** (1S), JST-GH connector J1 |
| **Solar cell** | SUNYIMA mini cell, **5 V / 30 mA (≈150 mW peak)**, 53 × 30 mm polycrystalline — [Amazon.de B09YH5SFQC](https://www.amazon.de/dp/B09YH5SFQC) (10-pack; one cell used) |
| **Charger** | CN3165 (U8), VIN = max(VUSB, VSOL) via D5/Q2 diode-OR |
| **Charge current** | R12 (ISET) = 2.7 kΩ → `I_CH = (2 V / 2.7 kΩ) × 986 ≈ 730 mA` from USB; **from solar the current is panel-limited to ~20–25 mA** — the linear charger simply takes what the panel gives, no MPPT |

Energy balance: the tracker at 2 h interval needs ~6 mAh/day ≈ 22 mWh.
Even **30–60 min of direct sun** (≥ 20 mA into the battery for that time)
covers it; at 15 min interval (~25 mAh/day) roughly 1.5–2 h of sun is
needed. The 30 mA panel is tiny next to the 730 mA ISET setting — that is
fine (charging is just slow), but it also means **the panel alone will never
trigger the charger's termination logic at full current; it tops up slowly
and relies on the CN3165's recharge behavior.** Also note P4 synergy: while
the panel charges the cell above 3500 mV, the firmware automatically leaves
degraded mode.

## 2. What was implemented (v2.1.0)

### P1 — GPS strategy (`app/App.cpp` + `DiagStore`)
- **Warm-start budget** `CONFIG_MINILORA_GPS_WARM_ATTEMPTS` (default 15 s):
  every wake is a hot/warm start (backup mode retains ephemeris + time), so
  the long window is only for true cold boots (power-on, 150 s).
- **Indoor backoff**: consecutive failures (persisted in NVS) make GPS run
  only every `2^fails` cycles (fails clamped to 3 ⇒ every 8th); off-cycles
  skip the GNSS entirely (stays in 15 µA backup). A success clears the
  counter. A stored/indoor tracker stops burning 0.6–1.5 mAh per cycle.
- **TTFF logged** every fix → tune the 15 s default from real data.

### P3 — Field profile
- **BLE beacon default OFF** (`CONFIG_MINILORA_DEFAULT_BEACON`, default n):
  LoRaWAN carries the data; the config service (long-press) is unaffected.
  Flip the Kconfig for phone-visible deployments. (A per-device beacon flag
  would need payload/ABI v3 — deliberately deferred.)
- **ADR explicit** (`node->setADR(true)`, RadioLib default): the network
  adapts datarate/TX power as the tracker settles near gateways.
- Static TX power deliberately *not* forced — ADR governs it.

### P4 — Low-battery autonomy (`CONFIG_MINILORA_BAT_*`)
| State | Threshold | Behavior |
|---|---|---|
| Normal | ≥ 3500 mV | full cycle |
| **Degraded** | < 3500 mV | no BLE beacon, no Zigbee, no WiFi, GPS every other cycle, **sleep ×2** (clamped 120 min) |
| **Critical** | < 3300 mV | GPS off, BME280 only, LoRaWAN heartbeat only, sleep = 120 min |

Config mode (long-press) always remains available. The payload's
`SleepMinutes` byte keeps reporting `cfgRange` (payload v1 is frozen); the
degraded/critical flags ride the **diagnostics uplink bytes 20–21**
(`gpsConsecFails`, `powerFlags`) — frame still 22 B.

## 3. Expected effect

| Scenario | v1 / v2.0 | v2.1.0 |
|---|---|---|
| Outdoor, 15 min, warm fixes 10 s | ~36 mAh/day | **~25 mAh/day** |
| Indoor/stored (GPS never fixes) | ~40–70 mAh/day | **~6 mAh/day** (backoff) |
| Dying battery | dead | **months of heartbeats**, findable via last-known config |

## 4. Validation checklist

- [ ] Host tests (diag layout incl. bytes 20/21) — ✅ 85 green
- [ ] Bench TTFF session → calibrate `MINILORA_GPS_WARM_ATTEMPTS` (use the
      probe/backup/restore flow; expect 1–10 s warm)
- [ ] Simulate low battery (set `BAT_LOW_MV` absurdly high) → observe doubled
      sleep + skipped GPS in the serial log
- [ ] Simulate indoor (no antenna) → observe backoff cycling 1→2→4→8
- [ ] Joulescope deep-sleep current (P6, Phase 7)
- [ ] Solar charge test: panel in sun, watch `batMv` climb in the serial log
      and confirm the device leaves degraded mode (thresholds vs charge
      voltage interplay)
