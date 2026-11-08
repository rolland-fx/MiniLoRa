# DOC — datasheets & schematics

Datasheets for every part on the current board (Beta 5), the modules it is
built from, and the candidates for the next revision
([HW/Next_HW_version.md](../HW/Next_HW_version.md)).

## On-board parts (Beta 5)

| File | Part | Role |
|---|---|---|
| [Datasheets/ESP32-C6-MINI-1.pdf](Datasheets/ESP32-C6-MINI-1.pdf) | ESP32-C6-MINI-1 | the SoC module (U7) |
| [Datasheets/esp32-c6.pdf](Datasheets/esp32-c6.pdf) | ESP32-C6 | the chip — deep sleep, RTC/LP pins |
| [Datasheets/Wio-SX1262_Module.pdf](Datasheets/Wio-SX1262_Module.pdf) | Wio-SX1262 | the LoRa module (U2) |
| [Datasheets/SX1261_2 V2-2-1.pdf](Datasheets/SX1261_2%20V2-2-1.pdf) | SX1261/2 | the LoRa transceiver inside it |
| [Datasheets/CD-PA1010D.pdf](Datasheets/CD-PA1010D.pdf) | PA1010D | the GNSS module (U3) |
| [Datasheets/MT3333.pdf](Datasheets/MT3333.pdf) | MT3333 | the GNSS chipset inside it |
| [Datasheets/bst-bme280-ds002.pdf](Datasheets/bst-bme280-ds002.pdf) | BME280 | temperature / humidity / pressure (U6) |
| [Datasheets/lis3dh.pdf](Datasheets/lis3dh.pdf) | LIS3DH | 3-axis accelerometer (U4) |
| [Datasheets/opt3001.pdf](Datasheets/opt3001.pdf) | OPT3001 | ambient light (U5) |
| [Datasheets/AO3400A.pdf](Datasheets/AO3400A.pdf) | AO3400A | N-FET load switches (Q1/Q3) |
| [Datasheets/SGM6029.pdf](Datasheets/SGM6029.pdf) | SGM6029 | 2.3 µA-Iq synchronous buck — the 3.3 V rail (U9) |
| [Datasheets/CN3163.pdf](Datasheets/CN3163.pdf) | CN3163 | USB-side Li-ion charger (U) — its exact battery-drain spec remains an open question (HW doc §9.7) |
| [Datasheets/CN3165.pdf](Datasheets/CN3165.pdf) | CN3165 | solar Li-ion charger (U8) |

## Next-hardware candidates

| File | Part | Role |
|---|---|---|
| [Datasheets/MAX-M10S.pdf](Datasheets/MAX-M10S.pdf) | MAX-M10S | the next GNSS (reference circuit in §2.5) |
| [Datasheets/Wio-S3.pdf](Datasheets/Wio-S3.pdf) | Wio-S3 | the next SoC + LoRa module (§1) |

## Heritage / reference (the Beta-5 carrier)

| File | Role |
|---|---|
| [Datasheets/XIAO-ES32C6.txt](Datasheets/XIAO-ES32C6.txt) | Seeed XIAO ESP32-C6 pinout notes — the carrier layout this board inherits |
| [Schematics/XIAO ESP32 C6_v1.0_SCH_PDF_24028.pdf](Schematics/XIAO%20ESP32%20C6_v1.0_SCH_PDF_24028.pdf) | the XIAO schematic |

Still worth exporting from KiCad: a **PDF of the board's own schematic**
(`MiniLora.kicad_sch`) — right now it exists only as source.
