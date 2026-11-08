# MiniLoRa Payload Format

**Length:** 28 bytes (fixed)
**Encoding:** Little-Endian for multi-byte fields (ESP32 native order)

```
Byte     Field           Type        Decode
─────────────────────────────────────────────────────
0        Temp            int8_t      Direct °C value
1        Hum             uint8_t     % relative humidity
2-3      Pressure        uint16_t    0.1 hPa units (deci-hPa, little-endian)
4        Bat_vol         uint8_t     Linear mapping: 2500mV→0, 4300mV→255
5-6      Luminosity      uint16_t    Lux (little-endian)
7        Acc_x           uint8_t     Map: -10000→0, 10000→255
8        Acc_y           uint8_t     Map: -10000→0, 10000→255
9        Acc_z           uint8_t     Map: -10000→0, 10000→255
10-13    Latitude        int32_t     Microdegrees: actual × 1 000 000 (little-endian, signed)
14-17    Longitude       int32_t     Microdegrees: actual × 1 000 000 (little-endian, signed)
18-19    Altitude        int16_t     Meters + 1000 offset (little-endian): altitude = raw - 1000
20       Satellites      uint8_t      Count of satellites with fix
21-24    Time            uint32_t    Unix epoch seconds (little-endian)
25       AwakeSec        uint8_t      Seconds since device woke up
26       SleepMinutes    uint8_t      Deep sleep duration in minutes
27       GPIOStatus      uint8_t      Bitmask: bit0=GPIO6(LED_GREEN), bit1=Button_2
```

## Example Hex → Values

> Regenerated in Phase 0 by `Firmware/tools/gen_vectors.py` so the hex and the
> table are mutually consistent (the original example had hand-computed
> arithmetic errors). See `Firmware/docs/GOLDEN_VECTORS.md`.

```
Hex: 1A 0F F9 03 F5 2F 04 7F 7F 7F D2 C6 29 03 A4 7E 87 00 E5 03 08 6B 19 5D 6A 4B 0F 01
```

| Field    | Raw     | Calculated                           |
|----------|---------|--------------------------------------|
| Temp     | `1A`    | 26 °C                               |
| Hum      | `0F`    | 15 %                                |
| Pressure | `BA 27` | 0x27BA = 10170 → 1017.0 hPa         |
| Bat_vol  | `F5`    | 245 → voltage ≈ 4.23 V              |
| Lum      | `2F 04` | 0x042F = 1071 lux                   |
| Acc_x    | `7F`    | 127 → ≈ 0 g (raw ≈ −39 counts)      |
| Acc_y    | `7F`    | 127 → ≈ 0 g (raw ≈ −39 counts)      |
| Acc_z    | `7F`    | 127 → ≈ 0 g (raw ≈ −39 counts)      |
| Lat      | `D2 C6 29 03` | 0x0329C6D2 = 53 069 522 → 53.069522 °N |
| Lon      | `A4 7E 87 00` | 0x00877EA4 = 8 879 780 → 8.879780 °E  |
| Alt      | `E5 03` | 0x03E5 = 997 → 997-1000 = -3 m      |
| Sat      | `08`    | 8 satellites                        |
| Time     | `6B 19 5D 6A` | 0x6A5D196B = 1 784 486 251 = 2026-07-19 18:37:31 UTC |
| Awake    | `4B`    | 75 seconds                          |
| Sleep    | `0F`    | 15 minutes                          |
| GPIO     | `01`    | Bit0=1 (GPIO6 HIGH), bit1=0         |