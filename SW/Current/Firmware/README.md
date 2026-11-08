# MiniLoRa Firmware v2

ESP-IDF v5.5 firmware for the MiniLoRa ESP32-C6 board (SX1262 LoRa, PA1010D GNSS,
BME280, OPT3001, LIS3DH). Rewritten from the Arduino concept firmware
(tag `Before-Rework`) — see `../../../Roadmap.md` for the plan and `docs/` for
per-phase implementation notes.

## Build

```bash
. ~/.espressif/frameworks/esp-idf/export.sh
idf.py set-target esp32c6   # first time only
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Layout

```
main/            application (app/, domain/, drivers/, transports/)
host_tests/      host-side unit tests (no ESP-IDF needed) + golden vectors
tools/           generators (golden vectors, etc.)
docs/            per-phase design notes
```

## Host tests

```bash
cd host_tests && cmake -B build && cmake --build build && ./build/minilora_tests
```

## Configuration

Device identity, WiFi and LoRaWAN credentials live in NVS (written over BLE by
the Android configurator). Build-time defaults come from Kconfig; secrets are
never committed — see `docs/PHASE2_DOMAIN.md`.
