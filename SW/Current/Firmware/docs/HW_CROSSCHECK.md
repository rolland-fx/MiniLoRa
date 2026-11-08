# HW ↔ FW Cross-Check — Production Board

**Date:** 2026-10-31 · **Method:** netlist analysis of `HW/MiniLora.kicad_pcb`
(ESP32-C6-MINI-1U pad map decoded against the official datasheet) **+ on-device
I2C/GPIO probe** flashed over a full-flash backup of the running v1 firmware
(backup restored and verified byte-identical afterwards).

## Probe results (real device, `/dev/ttyACM0`)

```
[SCAN] responding addresses: 0x10 0x18 0x47 0x76
[LIS3DH]  0x18: ACK who_am_i=0x33  <== LIS3DH HERE
[LIS3DH]  0x19: NACK
[BME280]  0x76: ACK id=0x60        <== BME280 HERE
[OPT3001] 0x47: ACK dev_id=0x30xx  <== OPT3001 HERE
[PA1010D] 0x10: ACK
[PPS_INT/GPIO4] edges in 3s: 0 (quiet)
[RF_SW/GPIO3] idle level: 0
[BATTERY] BAT_A avg = 1677 mV => Vbat = 3354 mV (x2 divider)
```

## Verdicts

| # | Finding | Status | Action |
|---|---|---|---|
| 1 | Pin map (SX1262 17/18/19/20/1/2/16, GNSS wake 0, BAT 21/5, I2C 22/23, LEDs 15/6, BP1 7, BP2 14) | ✅ matches v1 FW and v2 `Pins.hpp` | none |
| 2 | Battery divider 100k/100k = ×2 | ✅ verified (3354 mV plausible) | none |
| 3 | BME280 = 0x76 (SDO GND), OPT3001 = 0x47, PA1010D = 0x10 | ✅ | none |
| 4 | **LIS3DH = 0x18, NOT 0x19** — SDO/SA0 (pin 7) tied GND; v1 probed 0x19 and never talked to it | 🔴 confirmed by WHO_AM_I | **v2 fixed** (`kAddrAcc = 0x18`); note the README table (0x18) was right and the v1 code was wrong |
| 5 | **LORA_RF_SW on GPIO3 never driven** — module datasheet: RF_SW high = RX path enabled; DIO2 (internal, not on footprint) drives TX | 🔴 confirmed: idle level 0 → v1 RX path never enabled | **v2 fixed**: `setDio2AsRfSwitch(true)` + GPIO3 held HIGH during the radio session, parked LOW after |
| 6 | **PPS_INT (GPIO4) shared by GPS PPS + LIS3DH INT1 + OPT3001 INT, no pull-up** | 🟠 structurally risky | **v2 mitigated**: LIS3DH CTRL_REG5 D4D_INT1 = open-drain; rule: never arm GPIO4 as wake source while GPS is awake (PPS toggles 1 Hz) |
| 7 | LEDs D1/D6 orientation | ✅ resolved by user: production boards have LEDs the other way, LED1/LED2 work — v1 polarity (blue active-LOW, green active-HIGH) is correct | none |
| 8 | GPIO14 (BP2+EIO2) is the *external button*, not an antenna switch | ✅ confirmed — v1 gpioStatus bit1 semantics are correct | comment fixed in `Pins.hpp` |
| 9 | SGM6029C buck, R10 = 249 kΩ | ✅ → VDD = 3.3 V (16-level VSEL table) | none |
| 10 | Flash = 4 MB (C6-MINI-1 embedded) | ✅ partition table OK | none |

## Open items (visual check in KiCad recommended)

- **OPT3001 ADDR bump**: the footprint shows *two pads on SCL* and no clear
  ADDR connection — yet 0x47 answers, so it works; still worth confirming the
  bump map for production robustness.
- **CN3165 charger input path** (Q2 solar ideal-diode, D5 USB Schottky): the
  netlist is consistent with "VIN = max(VUSB, VSOL)" but the pin roles should
  be eyeballed in the schematic. Netlist-confirmed: U8 pin 4 (VIN) shares
  `Net-(D5-K)` with Q2 and D5; U8 pin 2 (ISET) → R12 (2.7 kΩ) → GND.
- **CN3165 charge current vs sources**: R12 = 2.7 kΩ programs
  `I_CH = (2 V / 2.7 kΩ) × 986 ≈ 730 mA` — fine from USB (0.66 C on the
  1100 mAh 1S cell, J1), but the connected SUNYIMA solar cell
  ([B09YH5SFQC](https://www.amazon.de/dp/B09YH5SFQC), 5 V / 30 mA,
  53 × 30 mm) can only source ~20–25 mA: solar charging is panel-limited
  and slow by design (no MPPT on a linear charger). Verify on the bench
  that the panel voltage does not collapse below the CN3165's VIN
  under-voltage lockout in low light (it would simply stop charging —
  harmless, but good to know for winter deployment).
- **GPIO15 strapping**: connected only to a reverse-biased LED → floating at
  reset (internal weak pull-up probably fine).
- **ADC source impedance**: 200 kΩ total on BAT_A — v1-compatible but verify
  accuracy on the bench (probe read 3354 mV; compare with a multimeter).

## Flash backup

The v1 image was backed up before probing and restored + verified:

```
/tmp/opencode/v1_flash_backup.bin  (moved to ~/Documents/Git/MiniLoRa_v1_flash_0fd44bbd.bin)
md5 = 0fd44bbd991d24cb42a8f4becd7052d8  (read-back after restore: identical)
```
