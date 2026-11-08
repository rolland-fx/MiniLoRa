# MiniLoRa Next Hardware Version — Planning Document

**Status:** draft / discussion · **Date:** 2026-11-02 · **Updated:** 2026-11-05 (§2.5 GNSS circuit, §5 sensor plan, §6 RTC pins, §7 power) · **Current HW:** Beta 5 (XIAO ESP32-C6 carrier + external SX1262 + PA1010D GPS)

This document collects the plan for the next hardware revision, based on the
evaluation of two changes:

1. **SoC + LoRa:** replace the ESP32-C6 module + external SX1262 radio with a
   single **Seeed Wio-S3** module (ESP32-S3R8 + SX1262 integrated).
2. **GNSS:** replace the PA1010D (thick embedded patch antenna) with a
   **u-blox MAX-M10S** (LCC, RF pad → thin external antenna).

Both are drop-in *conceptually* — the firmware already drives an SX1262 via
RadioLib and parses NMEA via minmea — but each has migration details that must
be respected. Everything below is grounded in the datasheets and in the
current `main/drivers/Pins.hpp` pin map.

---

## 1. Candidate module: Seeed Wio-S3

**SKU:** 100020327 (with IPEX) / 100079384 (without) · **~$7.99**

| Item | Specification |
|---|---|
| MCU | ESP32-S3R8, dual-core Xtensa LX7 @ 240 MHz |
| Radio | Semtech SX1262 (EU868/US915/…), TCXO integrated, 20.9 dBm @ 868 MHz |
| Sensitivity | −137 dBm @ SF12 125 kHz |
| Memory | 16 MB flash + 8 MB PSRAM (S3R8 = 512 KB SRAM + 8 MB octal PSRAM) |
| Connectivity | 2.4 GHz 802.11b/g/n Wi-Fi, BLE 5.0 |
| Package | 38 SMT pins, 21.6 × 16.5 × 3.3 mm |
| Power | 3.0–3.6 V; deep sleep 9.3 µA (module total); 113–127 mA TX |
| Interfaces | 3× UART, I2C, SPI, ADC, native USB, RST/BOOT |
| Temp | −40 … +85 °C |
| Certification | FCC, CE, TELEC (module-level) |

### 1.1 What carries over unchanged

| Subsystem | Why it carries over |
|---|---|
| **LoRaWAN stack** | RadioLib `SX1262` + `LoRaWANNode` runs on ESP32-S3. The current driver already uses RadioLib (`LoRaWanTransport.cpp`: `SX1262 *radio`, `EspHal`, SPI2_HOST @ 8 MHz). Port = new pin numbers + DIO3-TCXO argument. |
| **Payload / ICD ABI** | fPort 1/2/3/4 frames are frozen; nothing consumer-side changes. |
| **BLE config service + beacon** | S3 BLE 5.0 supports coded PHY; the CAFE/CAFF envelope and the 28 B beacon are unchanged. NimBLE works on S3. |
| **WiFi push / OTA pipeline** | Same 802.11n 2.4 GHz; the server flow (upload → activate → notify → device pull) is unchanged. |
| **Sensors** | BME280 (0x76), LIS3DH (0x18), OPT3001 (0x47), GPS NMEA over I2C — all just I2C devices. |
| **Store-and-forward buffer** | NVS ring (50 × 28 B) carries over; on S3 it can scale to thousands of points (see §4). |

### 1.2 What is lost

| Loss | Impact | Mitigation |
|---|---|---|
| **Zigbee / 802.15.4** (C6 has the radio, S3 does not) | The optional Zigbee transport into HA is gone | HA is already covered by the server's MQTT-discovery bridge. `ZigbeeTransport` is deleted → **smaller binary** (welcome: the app slot has only ~51 KB free). Keep the diag counter for compatibility. |
| Wi-Fi 6 (C6) / Thread & Matter | None — unused | — |
| RF switch GPIO (current board has an external 2.4 G/LoRa antenna switch on GPIO3) | The Wio-S3 integrates the SX1262; check whether the module exposes ESP-side RF for a switch | Verify in the datasheet pinout; likely each radio gets its own antenna (LoRa IPEX + ESP 2.4 G pad/connector). |

### 1.3 What is gained (ranked for this project)

1. **Store-and-forward at scale** — 8 MB PSRAM / 16 MB flash turns the 50-point
   NVS ring into **~100,000 points** (weeks of offline logging). The Buffer
   module scales by changing one constant + moving storage.
2. **Real on-device data logging** — months of full-resolution history in
   flash (LittleFS), queryable over BLE/WiFi; fPort-4 history grows from
   8 slots to thousands.
3. **OTA-slot pressure gone** — the current 2 × 1.9 MB slots become
   2 × ~6 MB (today's 1.85 MB bin leaves only ~51 KB headroom).
4. **Dual-core 240 MHz** — GPS NMEA parsing / LoRa IRQ / sensor sampling can
   overlap; the awake window could *shrink* (directly saves battery).
5. **Native USB (OTG)** — flashing/logging/config without UART.
6. **Certification** — module-level FCC/CE/TELEC skips RF testing if this
   becomes a product.
7. **Cost** — $7.99 vs C6 module + SX1262 module + TCXO + crystals.

### 1.4 Firmware migration checklist

- [ ] `idf.py set-target esp32s3` + **full sdkconfig regeneration** (every
      `CONFIG_*` re-checked; keep `CONFIG_MINILORA_FW_VERSION` mechanism)
- [ ] **Pin remap** — the SX1262 pins are fixed by the module:
      `LoRa_CS=GPIO21, LoRa_SCK=GPIO4, LoRa_MOSI=GPIO6` (MISO/DIO1/BUSY/RESET:
      see Wio-S3 datasheet "SX1262 Pin Mapping" table). Update `Pins.hpp`.
- [ ] **RadioLib TCXO**: `SX1262` constructor needs the DIO3-as-TCXO
      configuration (1.8 V) — the current external module may already use
      this; verify the Wio-S3 TCXO voltage in the datasheet.
- [ ] **RTC-capable GPIOs for wake sources** — S3 EXT1 wake only works on
      GPIO 0–21: BP1 (currently GPIO7 ✓), motion INT, and the future button
      must land there. BP2 is awake-only today and can stay on any pin.
- [ ] **Delete `ZigbeeTransport`** (and the `zb_storage` FAT partition need),
      keep the diag counter field for ABI stability.
- [ ] **Partition table for 16 MB**: `nvs` (keep 24 KB or grow),
      `phy_init`, `otadata`, `ota_0` + `ota_1` (~6 MB each),
      `data` (LittleFS for logging), `coredump`.
- [ ] **Server**: `/api/ota/upload` hardcodes the `0x1D0000` size cap — make it
      per-device (partition size from the device record).
- [ ] **Deep-sleep GPIO/EXT1 config re-test** on S3 (boot-strapping pins:
      GPIO0/46 strapping; GPIO0 is BOOT).
- [ ] Re-verify the **PSRAM power** is disabled in deep sleep (adds ~µA if on).
- [ ] Re-run the **host tests** (unchanged — pure logic) and the bench
      current-measurement session (Joulescope, P6 backlog item).

### 1.5 Hardware migration checklist (PCB)

- [ ] Footprint: 38-pin SMT castellated, 21.6 × 16.5 mm; KiCad lib exists
      (Seeed provides `.kicad_mod` + STEP).
- [ ] **2.4 GHz antenna routing**: confirm from the datasheet whether the
      ESP-S3 RF goes to a pin, a second IPEX, or an onboard PCB antenna —
      this decides the enclosure layout.
- [ ] LoRa antenna: IPEX (SKU 100020327) → same external antenna as today.
- [ ] **32.768 kHz crystal for the RTC** (2026-11-03: the firmware now sets
      the system clock from GPS/DeviceTimeAns and reads it every cycle —
      with the internal RC the per-cycle re-sync masks the drift, but the
      crystal keeps an unanchored day honest for cents).
- [ ] Power: single 3.3 V rail (500 mA capable — SX1262 TX + ESP TX peaks);
      the existing battery/charger chain carries over.
- [ ] GPS V_BCKP on the always-on rail (see §2.3).
- [ ] Buttons/LEDs: BP1 on RTC GPIO (wake), BP2 anywhere, LED on any pin.

### 1.6 Risks

| Risk | Mitigation |
|---|---|
| RadioLib on S3 + ESP-IDF (current build uses `EspHal`) | Already works on C6/IDF; S3 is a supported RadioLib target. Prototype on a dev board first. |
| Deep-sleep current higher than C6 (9.3 µA module vs ~7 µA C6 + SX sleep) | ~1–3 µA difference is a few % of battery life at the 15-min duty cycle; measure on the prototype before committing. |
| S3 strapping pins (GPIO0/3/45/46) conflict with sensors at boot | Choose sensor pins outside the strapping set or add series resistors. |
| PSRAM/flash heat in a sealed enclosure | -40…85 °C rating; measure in the Joulescope session. |
| Seeed single-sourcing | The design is standard ESP32-S3 + SX1262; a pin-compatible fallback exists (Wio-SX1262 + separate S3, or LILYGO T-S3 boards). |

---

## 2. Candidate GNSS: u-blox MAX-M10S

**Motivation:** the PA1010D module's embedded ceramic patch antenna is thick;
the module also draws ~25 mA during acquisition — the largest energy item
after the radio. The MAX-M10S halves that and pairs with *any thin antenna*.

### 2.1 Comparison (datasheet values)

| | PA1010D (current, MT3333) | **MAX-M10S** | Quectel LC29H (dual-band) |
|---|---|---|---|
| Constellations | GPS + SBAS | **GPS+Galileo+BeiDou+GLONASS concurrent** | GPS+…+ **L1+L5** |
| Acquisition current | ~25 mA | **8 mA (GPS) – 13 mA (4 GNSS)** @ 3.0 V (peak 25 mA) | ~30+ mA (verify) |
| Cold start | ~30 s | **24 s** (aided 2 s, hot 1 s) | fast in urban (L5) |
| Sensitivity | −148 dBm-ish cold | **−148 dBm cold / −167 dBm tracking** | good (L5) |
| Interface | I2C (NMEA) | **I2C DDC (NMEA)** + UART + SPI | UART (+I2C — verify) |
| Package | module w/ thick patch | **18-pin LCC 9.7 × 10.1 × 2.5 mm, RF_IN pad** | LCC, RF pad |
| Antenna bias | — | **VCC_RF out + LNA_EN** (active antennas supported) | VCC_RF |
| Price | ~$10 (breakout) | ~$15–20 (module) | ~$8–12 |
| Verdict | — | **recommended** | pick only for dense-city tracking |

Why the MAX-M10S wins here: 2–3× less acquisition energy (the GPS phase is
the dominant power item after the radio), 4 constellations = more
satellites = faster fixes with the 8-sat minimum, I2C keeps the NMEA/minmea
driver, and the RF pad accepts any thin antenna.

### 2.2 Thin antenna options (the thick patch is the complaint)

| Option | Thickness | Notes |
|---|---|---|
| **FPC / LDS flex antenna** | ~0.2 mm | tape-in; best for the enclosure; add SAW+LNA if long cable |
| **Chip antenna** (Taoglas/Abracon) | 2–3 mm | needs ground clearance per datasheet |
| **u.FL to active antenna** | connector only | puck is external; fine when stationary |

The M10's `VCC_RF` (≈ VCC − 0.1 V, 50 mA capability) + `LNA_EN` support
active antennas natively.

### 2.3 V_BCKP on the always-on rail (the real energy win)

**Design:** load switch on the GNSS `VCC`; `V_BCKP` fed from the always-on
3.3 V (optionally through a supercap). Between cycles the firmware switches
VCC **off** entirely — hardware backup mode (28–46 µA) retains ephemeris,
almanac and the RTC.

Consequences:

- Every wake becomes a **warm start (1–2 s)** instead of a 24 s cold start →
  the awake window shrinks; battery savings exceed the radio difference.
- The RTC keeps the **UTC date** — this prevents the entire "position but no
  date" bug class fixed in fw 2.6.3 (a cold-started receiver has no GPS week
  number until it decodes subframe 1; the RMC date then trails the first
  position fix by tens of seconds).
- Backup current is paid 24/7: 28–46 µA vs PA1010D deep sleep — net win
  because cycles shorten by ~20–30 s each.

### 2.4 Firmware port

- New `MaxM10s` driver class replacing `Pa1010d`: same `GpsFix` struct, same
  minmea parsing, same `poll()` interface. Changes only:
  - standby: **UBX-RXM-PMREQ** (or the VCC load switch) instead of
    `PMTK` deep-sleep;
  - optional: disable NMEA sentences not needed (GSA/GSV/VTG) to cut I2C
    traffic; keep RMC+GGA;
  - the config is persisted in the module's backup RAM → send once, not per
    session.
- `kAddrGnss` changes (u-blox DDC default address **0x42**).
- `kGnssWake` GPIO becomes the load-switch enable (active HIGH = powered).
- The `kMinSatellites = 7` constant inside the driver must be aligned with
  the App-level `kGpsMinSats = 8` (fw 2.6.3) — or lowered to 3 and let the
  App gate decide (single source of truth).

### 2.5 MAX-M10S reference circuit (simple but efficient)

Design rules: reuse parts already on the Beta 5 BOM (AO3401A P-FET
`C15127`, LMBR4010 Schottky `C2912031`), no new regulator, no RF magic
beyond one 50 Ω trace. Every control pin lands on an RTC GPIO (§6).

| Net | Circuit |
|---|---|
| **VCC** | 3V3 rail → high-side P-FET AO3401A (source=3V3, drain=VCC); gate pulled **up** 100 kΩ to 3V3 (default OFF), an RTC GPIO drives the gate **LOW** through 1 kΩ to switch ON. GPIO level holds through sleep via `rtc_gpio_hold`. Input 4.7 µF + 100 nF at VCC. |
| **V_BCKP** | 3V3_ALWAYS → LMBR4010 Schottky → V_BCKP, 1 µF + 100 nF. Always powered (§2.3). DNP option: 0.1–1 F supercap in place of relying on the rail. |
| **I2C DDC** | address 0x42, onto the shared bus. Pull-ups stay on the always-on rail — with VCC switched off, back-power through the module's ESD diodes is ~µA; **measure** (Joulescope), gate the pull-ups only if it shows up. |
| **TIMEPULSE** | → its **own** RTC GPIO (replaces the PA1010D PPS on the shared PPS_INT net — no more sharing, see §6). Configure via UBX CFG-TP5; config persists in backup RAM. |
| **EXTINT** | → RTC GPIO (optional, DNP-able): aided start / precise time-mark later. |
| **RESET_N** | 10 kΩ to VCC + 100 nF to GND. No GPIO needed. |
| **SAFEBOOT_N** | 100 kΩ to GND — never float. |
| **Antenna** | RF_IN → 50 Ω coplanar waveguide → **u.FL**. `VCC_RF` (VCC−0.1 V, ≤50 mA) through a 600 Ω ferrite = active-antenna bias; `LNA_EN` to a GPIO if the antenna wants it. Passive FPC antenna is the default (§2.2). |
| **Layout** | RF trace < 15 mm to the u.FL, no copper in the RF-pad keep-out, ground stitching vias, 4-layer JLC stackup, module GND paddle on a via field. |

If the LCC is intimidating: an M10S breakout behind u.FL works as a
fallback — but this circuit is ~1 cm² and one JLC assembly run.

---

## 3. What does NOT change

- Payload v1 (28 B fPort 1), diagnostics v4 (28 B fPort 2), downlink commands
  (3 B fPort 3), position history (fPort 4), BLE config envelope (v3/v4) —
  the ICD is untouched by both switches.
- The server: ingest paths, OTA pipeline, prune, analytics, HA bridge. Only
  the OTA upload size cap needs generalizing.
- The Android app: BLE ABI and the TTN/WiFi APIs are identical.
- The store-and-forward buffer, the GPS date-hold rule (position+date or
  neither), and the prune watermark — all carry over as-is.

## 4. New-feature backlog enabled by the switch

| Feature | Enabled by | Effort |
|---|---|---|
| Offline logging: 50 → 100k+ points | 8 MB PSRAM / 16 MB flash | low (Buffer constant + storage move) |
| On-device month-scale history + query | 16 MB flash + LittleFS | medium |
| Faster cycles (dual-core overlap) | 240 MHz dual core | medium |
| Motion classification / fall detection (TinyML on LIS3DH) | 8 MB PSRAM + LX7 vector ext | medium-high |
| USB config/logging | S3 native USB | low |
| Display / more buttons / second I2C bus | 38-pin header | as needed |

## 5. Sensor plan — keep / swap / add (small PCB, µA sleep, fast awake OK)

**Framing:** awake power is fine if the awake window stays short — every
sensor read is already ms-scale (BME forced ~15 ms, LIS3DH 4×5 ms, OPT3001
≤800 ms integration — consider shortening to 100 ms). The only *slow* awake
items are GPS acquisition (fixed by the §2.3 warm starts) and the radio.
So the design question is purely: **what draws µA while asleep, and what
fits ~1 cm² of PCB.**

### 5.1 Where the sleep µA go today (Beta 5, JLCPCB BOM `JLCPCB/MiniLora-JLCPCB.xls`)

| Consumer | Sleep draw | Note |
|---|---|---|
| ESP32-C6-MINI-1 deep sleep | ~7 µA | module figure |
| SGM6029 buck (`C5153500`) Iq | **2.3 µA** | synchronous buck, PSM — already ideal; keep |
| CN3163 + CN3165 chargers, no input present | <3 µA **each** | family spec — up to 6 µA combined; measure, see §7 |
| LIS3DH parked (ODR=0) | ~2 µA | asset mode: ~4 µA with motion wake armed |
| BME280 sleep | 0.1–0.3 µA | forced reads per cycle |
| OPT3001 shutdown (`0xC010`) | ~1 µA | driver `shutdown()` verified |
| Battery divider (100k/100k) | 0 | switched by `kBatEnable`/GPIO21 |
| PA1010D standby (kGnssWake) | **unknown — measure** | biggest unknown; the §2.3 load switch + M10S makes it 0 µA VCC |
| Wio-SX1262 module sleep | ~1–2 µA (verify) | integrated in the Wio-S3 9.3 µA figure |

**≈ 12–18 µA total, plus the GPS standby unknown.** The next board's job is
to remove that unknown (GPS load switch), not to re-architect the rails.

### 5.2 Verdicts

| Sensor | Verdict | Why |
|---|---|---|
| **LIS3DH** (0x18) | **KEEP** | wake-on-motion path already built + ICD-documented (asset mode); ~2 µA parked, ~4 µA armed; ±4 g 10-bit is enough for motion/tilt/impact |
| **BME280** (0x76) | **KEEP** | 0.1–0.3 µA sleep with forced reads; no competitor does T+RH+P in one µA-class package |
| **OPT3001** (0x47) | **KEEP** | ~1 µA shutdown, true lux range; only real tweak: shorten integration to 100 ms in the driver |
| **Magnetic reed switch** | **ADD — recommended** | **0 µA**, ~2×5 mm, silent, sealed enclosure (no button holes!): EXT1 wake + the "BLE config" trigger (replaces long-press in a sealed case) + tamper detect. Wire to its own RTC GPIO (§6) |
| **MAX17048 fuel gauge** | **DNP option only** | 3–5 µA hibernate forever; the switched divider (0 µA) + server-side slope already gives voltage, trend and projection. Populate only if a product variant needs true SoC% |
| LIS2DW12 (1 µA class, 14-bit) | later variant only | not footprint-compatible with LIS3DH, needs a new driver — not for this revision |

**Rejected** (power or size, no use case): gas/AQ sensors (heater = mA),
magnetometer, microphone, any second MCU.

### 5.3 Wake-on-motion (P5) — already shipped; verify on the new board

Discovered 2026-11-05 while costing the power trade: the feature is
**fully implemented and deployed** in fw 2.6.16, gated behind asset mode
(`cfgAssetMode`, `cfgMotionSens` = 128/256/512 mg, burst budget
`cfgBurstMaxPerDay` = 20/day, `cfgBurstIntervalMin` = 5,
`cfgAssetHeartbeatH` = 24 — ICD §4 schema-4 body bytes). **No firmware
work is needed** — but the next board must keep it possible, and the
verification items below belong in the P6 bench session.

**Power cost** (LIS3DH 10 Hz LP ≈ 4 µA additive to ESP deep sleep):

| Sleep regime | Extra cost |
|---|---|
| 15 min cycle (current default) | +0.1–0.2 % — noise |
| 60 min | ~+5 % |
| 240 min heartbeat (asset) | +10–15 % — the price of "know it moved" vs a 4 h blind spot |

**Board requirements**

- **INT1 on its own RTC GPIO** — §6 already plans this; the EXT1 wake
  path (GPIO4 today) must survive the remap.
- **Keep INT1 open-drain** (`CTRL5 D4D_INT1`, already in the driver):
  on Beta 5 it shares `PPS_INT` with the push-pull GPS PPS; the M10S's
  TIMEPULSE gets its own pin (§2.5) so the sharing disappears — the
  open-drain config stays anyway (harmless, keeps the option).
- The burst budget is the energy guardrail: motion can never trigger
  more than 20 GPS cycles/day, spaced 5 min, heartbeat fallback else.

**Verification items** (full context: `docs/Next-Improv.md`)

1. **LIS3DH wake→read timing** — acc reads constant 133/133/133 across
   cycles smells like reading too early after wake (V1 used 25/50 ms
   settles). Load-bearing once the sensor is the wake source.
2. **Confirm the motion-burst path respects the critical-battery gate**
   — motion must not force a GPS cycle at battery cutoff.
3. **PPS contention edge** — GPS PPS enabled + INT1 low at the same
   instant briefly fights the push-pull driver; harmless (~100 ms), and
   the §2.5 pin split removes it.
4. **Field test**: asset mode on (config builder → Provision), sens=2,
   bench-shake → expect `wake=motion` + burst cycle; battery slope
   before/after on the analytics page validates the µA math above.

The rest of the derived-data backlog (server/app/on-board ideas —
delivery ratio via TTN `f_cnt`, battery projection, pressure trend,
payload-byte candidates) lives in **`docs/Next-Improv.md`**; none of it
constrains this PCB except the items above.

## 6. RTC-domain pin plan — best use of the ESP's RTC pins

**Principles**

1. **One wake source per RTC pin.** The shared `PPS_INT` net (GPS PPS +
   LIS3DH INT1 + OPT3001 INT on GPIO4) works but makes wake attribution
   ambiguous. The S3 has 22 RTC pins — no reason to share anymore.
2. **Anything whose LEVEL must persist through sleep goes on an RTC pin**
   (`rtc_gpio_hold`): GNSS load-switch enable, battery-divider enable, LEDs.
3. **Wake sources avoid strapping pins** (S3: GPIO0/3/45/46) and the
   module-fixed SX1262 pins (Wio-S3: SCK=GPIO4, MOSI=GPIO6, CS=GPIO21 —
   which is exactly why today's GPIO4 motion net must move).
4. **Awake-only signals go on cheap pins** (BP2, debug) — no RTC needed.
5. The **32.768 kHz crystal** (§1.5) stays on the RTC_XTAL pads — honest
   timebase through sleep for cents.

**Candidate S3 map** (verify against the Wio-S3 datasheet pinout —
open question):

| Function | Pin | RTC? | Notes |
|---|---|---|---|
| BP1 wake button | GPIO7 | ✓ | unchanged role, EXT1 |
| LIS3DH INT1 (motion wake) | GPIO5 | ✓ | dedicated — EXT1 bit, no sharing |
| GPS TIMEPULSE (PPS) | GPIO1 | ✓ | dedicated, timestamp-grade |
| Magnetic reed switch | GPIO2 | ✓ | EXT1 wake + config trigger |
| I2C SDA / SCL | GPIO8 / GPIO9 | ✓ | all sensors + GPS DDC |
| GNSS VCC load-switch enable | GPIO10 | ✓ | `rtc_gpio_hold` through sleep |
| Battery-divider enable | GPIO11 | ✓ | `rtc_gpio_hold` through sleep |
| LEDs (blue/green) | GPIO12 / GPIO13 | ✓ | hold-able |
| BP2 / external button | GPIO35+ (or any) | — | awake-only, as today |
| SX1262 (module-fixed) | CS=21, SCK=4, MOSI=6 + MISO/DIO1/BUSY/RESET | — | per Wio-S3 datasheet |
| Strapping — keep clear | GPIO0, 3, 45, 46 | — | 0 = BOOT |

## 7. Power-supply review — what to keep, what to change

**Already right — keep as-is** (verified against the JLCPCB BOM and the
datasheets):

- **SGM6029** (`C5153500`) main rail: 2.3 µA Iq synchronous buck, 0.6–1 A —
  better than any LDO swap; PSM mode is exactly the "µA sleep / fast awake"
  profile. Add 22 µF bulk at the output for radio TX peaks.
- **CN3165** (`C559035`) solar charger: solar-adaptive input current,
  <3 µA battery drain with no input — the board already has the solar path.
- **Switched battery divider** (AO3400 + 100k/100k): 0 µA in sleep.
- **AO3400/AO3401 load-switch pattern**: reuse the same FETs everywhere
  (GPS VCC per §2.5).

**Change / verify:**

1. **GPS VCC load switch** (§2.3/§2.5) — the single biggest sleep item
   removed (PA1010D standby → 0 µA), paid back by V_BCKP 28–46 µA and
   1–2 s warm starts (§2.3 math).
2. **Charger count**: CN3163 (USB) + CN3165 (solar) both tie to BAT — with
   no input present each leaks <3 µA → up to 6 µA combined. Options: keep
   both and measure; OR the two inputs into one CN3165 through the existing
   LMBR4010 Schottkys; or a power-path IC. Decide after the Joulescope run.
3. **Measure the unknowns** (the P6 Joulescope session): Beta 5 total
   sleep floor (does it match the ~12–18 µA estimate?), PA1010D standby,
   Wio-S3 module deep sleep (9.3 µA claim), Wio-SX1262 sleep, I2C
   back-power with the GPS VCC off.
4. **V_BCKP strategy A/B** on the prototype: always-on rail vs supercap —
   the §2.3 net-win math assumes warm starts; verify with real TTFFs.

## 8. Decision log

| Date | Decision |
|---|---|
| 2026-11-02 | Evaluated Wio-S3 as the next SoC+LoRa module; accepted direction (Zigbee loss accepted — HA covered by MQTT discovery). |
| 2026-11-02 | Evaluated GNSS replacements; MAX-M10S preferred over LC29H (power, I2C certainty) for this use case; LC29H revisited only for dense-city requirements. |
| 2026-11-02 | V_BCKP always-on + load-switched VCC adopted as the reference GPS power design. |
| 2026-11-02 | Prototype-first rule: port the firmware on a Wio-S3 dev board and measure deep-sleep current (Joulescope session) before cutting the next PCB. |
| 2026-11-03 | **Field verdict: the patch antenna cannot fix in the garden — hardware.** Two forced 150 s windows failed (08:47 on 2.6.4/min-8, 10:27 on 2.6.5/min-7), plus warm windows at 09:52 and ~11:12; diag counters (gps_timeouts 5→13, consecFails 5→7) account for every window. Wake path, parser, date rules all verified clean; the only fix in 24 h took 162 s TTFF (marginal even when it works). |
| 2026-11-03 | **Bench confirmation (USB, instrumented fw): the patch antenna sees 0–3 satellites.** 300 s narrated window: 306 GGA at 1 Hz, zero checksum failures, module/I2C/driver all healthy — but sats peaked at 3 (mostly 0) and the receiver never fixed. Even a min-4 gate would fail; the min-7/8 history is irrelevant. Patch antenna + placement is the limiter; MAX-M10S + thin external antenna (§2) confirmed. |
| 2026-11-05 | **Sensor plan adopted (§5):** keep LIS3DH + BME280 + OPT3001 (all µA-class, drivers proven); add a 0 µA magnetic reed wake/config switch on its own RTC GPIO; MAX17048 fuel gauge is a DNP option only (switched divider + server slope covers it). |
| 2026-11-05 | **RTC pin plan adopted (§6):** next board gives every wake source its own RTC GPIO (split the shared PPS_INT net); hold-able enables (GNSS switch, divider, LEDs) on RTC pins; strapping + module-fixed SX1262 pins avoided. |
| 2026-11-05 | **MAX-M10S reference circuit adopted (§2.5):** AO3401 load switch on VCC (BOM reuse), V_BCKP from the always rail via LMBR4010, TIMEPULSE on a dedicated RTC pin, 50 Ω → u.FL. |
| 2026-11-05 | **Power architecture (§7):** keep SGM6029 + CN31xx chain (already µA-class); add the GPS load switch; charger-consolidation decision deferred to the Joulescope measurements. |

## 9. Open questions

### 9.1 Field-verified gotchas (2026-11-03 OTA/GPS session)

- **Remote commands are RTC-resident.** A power cycle between "command
  delivered" and "command executed" silently cancels them (observed: the
  user's ~07:10 reboot ate the already-delivered 2.6.4 OTA command). TTN
  gives no warning; recovery is a one-line re-send.
- **An OTA reboot (esp_restart) also re-initializes RTC_DATA_ATTR.**
  Verified from field frames: the first two cycles after the 2.6.5 install
  carried honest epoch 0 — the net-time anchor (g_netEpoch) died with the
  reboot, and the DeviceTimeAns chain restored it two cycles later. Benign
  for the OTA itself (the flag is consumed before its own reboot), but any
  *other* pending RTC command would be cancelled by an OTA reboot too.
- **Design response for the next HW/firmware:** keep "pending remote
  command" and the net-time anchor in NVS (or mirror them there), so neither
  a power cycle nor an OTA reboot cancels a delivered command or loses the
  time reference. Cost is a few NVS writes per command — negligible.

1. How does the Wio-S3 route the **ESP 2.4 GHz antenna** (pin / second IPEX /
   onboard)? Decides the enclosure layout — check the datasheet pinout.
2. Does the reference "command firmware" on the module need to be **reflashed
   with our own** (yes — we ship our ESP-IDF app; the Seeed command firmware
   is only for their AT-style use).
3. Keep the **external RF switch** (current GPIO3) or rely on the SX1262
   DIO2 internal switch? Depends on the final antenna plan.
4. **LC29H I2C** NMEA support — verify only if the dual-band option is taken.
5. Battery/charger chain: unchanged, but re-verify the 500 mA peak budget
   (ESP + SX1262 TX simultaneous) against the chosen regulator.
6. **Wio-S3 datasheet pinout**: which RTC-capable GPIOs are actually
   exposed on the 38-pin header — finalizes the §6 candidate map (and
   answers the 2.4 GHz antenna routing question in §1.5).
7. **CN3163 battery-drain spec**: verify the CN31xx "<3 µA with no input"
   family claim on the USB charger's own datasheet (the §7 consolidation
   decision needs the real number).
8. **PA1010D standby current on Beta 5**: measure in the Joulescope
   session — the biggest sleep-budget unknown (§5.1); the M10S + load
   switch removes it either way, but the number sizes the win.
