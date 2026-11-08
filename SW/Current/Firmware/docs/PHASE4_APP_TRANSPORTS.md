# Phase 4 — Transports & Application

**Status:** ✅ builds green on ESP-IDF v5.5.1/esp32c6 (RadioLib v7.8.1 as a
managed component). **Hardware parity testing with the Android app is the
exit gate** — see checklist.

## Architecture

```
app/App.cpp            the state machine: wake → config? → sense → tx → sleep
app/Sleep.{hpp,cpp}    single owner of power transitions + watchdog feed
app/Stores.{hpp,cpp}   NVS config (v1-compatible namespace!) + diagnostics
transports/lorawan/    RadioLib SX1262 + LoRaWANNode (OTAA, EU868)
transports/ble/        NimBLE: coded-PHY beacon + config GATT service
transports/wifi/       esp_wifi STA + HTTP POST
```

`app_main` → `runCycle()` → **never returns**: the cycle ends in
`esp_deep_sleep_start()`, exactly like v1 (no `loop()` equivalent).

## Technical decisions

### RadioLib integration (the hard-won knowledge)
- RadioLib **is** published on the ESP component registry as
  `jgromes/radiolib` (^7.8.1) — `main/idf_component.yml` pulls it; CI needs
  no special steps.
- For non-Arduino builds RadioLib ships an **ESP-IDF HAL**
  (`src/hal/ESP-IDF/EspHal.h`) using `spi_master` — chip-agnostic, works on
  the C6. `RadioLib.h` auto-selects it when `ESP_PLATFORM` is defined and
  `ARDUINO` is not.
- The HAL **requires `CONFIG_FREERTOS_HZ=1000`** (already in our defaults)
  for accurate RX1/RX2 window timing.
- v7.8 API differences vs the v1 firmware (Arduino RadioLib v6):
  `begin(5, 0x12, 8)` — explicit CR 4/5, private sync word, preamble 8;
  `beginOTAA(joinEUI_u64, devEUI_u64, nwkKey*, appKey*)`.
- **No deletes of radio/node**: v7 classes have non-virtual destructors and
  the heap is discarded at deep sleep anyway. Pins are parked to reset state.

### LoRaWAN session persistence — v1 pattern kept
Nonces + session buffers live in `RTC_DATA_ATTR` (survive deep sleep, lost on
power cycle → one rejoin). NVS backup was rejected: ~1 write/cycle would wear
NVS at 5-minute intervals; RTC RAM is free. Same tradeoff as v1.

### BLE — full Android compatibility
- Beacon: extended advertising, **LE Coded PHY**, 28-byte payload as
  manufacturer data. v1's 10-second interval was a bug (units are 0.625 ms ⇒
  16000 = 10 s!); v2 uses 625 ms — 16× more beacons in the same awake window
  for negligible power.
- Config service `0000FACE…`/`0000CAFE…`: reads return the **legacy 158-byte
  blob** (stock Android parses exactly that); writes accept **158 or 159**
  bytes (159 = v2 with `cfgZigbee`). `fromLegacyBlob()` in the domain layer.
- Full NimBLE stack init/deinit per cycle (beacon ~2.5 s, config ≤120 s).

### NVS — v1 namespace preserved
`ConfigStore` uses namespace `"config"`, key `"cfg"` — **a device configured
by the v1 Android app keeps its settings after the v2 OTA**. Schema handling:
raw 158-byte blob = v1, `01||158B` = schema 1, `02||159B` = schema 2.

### Sleep policy (`app/Sleep.cpp`)
Single owner: timer wake (`kSleepTableMs[cfgRange]`), EXT1 on GPIO7 (any-low),
GPIO holds for GNSS wake/SX1262 reset/LEDs/battery enable, RTC isolation of
0/1/4/5. Feeding: `feedWatchdog()` = `esp_task_wdt_reset()`, called at every
phase boundary + inside the GPS retry loop (180 s task-WDT budget).

### Boot-loop guard (first cut)
`DiagStore::onBoot()`: expected resets = deep-sleep/poweron/software; anything
else increments `unexpectedResets`. >3 → **recovery mode**: BLE config window
+ minimal cycle, no radios TX, no sensors. Cleared on a clean completed cycle.
(Phase 6 extends this with fault-injection tests.)

### Downlink contract (unchanged)
fport 1 downlink: byte 0 = sleep index (0 = default, ≥1 <9 = index, else
ignored), byte 1 = GPIO6 control (bit0 override-enable, bit1 level). Applied
through `applyDownlink()` → NVS save + immediate `applyGpio6()`.

## v1 → v2 divergences (intentional)

| v1 | v2 | Why |
|---|---|---|
| 10 s BLE beacon interval | 625 ms | v1 passed 16000 to a 0.625 ms-unit API — likely a units bug |
| Silent GPS timeout → zeros | `no_fix` payload + `gpsTimeouts` counter | explicit failure path (Roadmap §3) |
| Secrets as compile-time defaults | empty defaults; configure via BLE | keys out of the binary/git |
| `map()` wrapping at range edges | saturation | documented golden vector `acc_saturation` |

## Exit checklist (hardware + Android)

- [ ] Flash v2; Android app sees the coded-PHY beacon and decodes values
- [ ] Long-press → config mode; v1 Android app writes 158 bytes → accepted
- [ ] Device keeps LoRaWAN session across 10 sleep cycles (no rejoin in TTN logs)
- [ ] Downlink sleep-index change applied + persisted
- [ ] WiFi POST received by `Wifi-Test-Server/server.py`
- [ ] No rejoin after a `Reset` from TTN console (session restored)
- [ ] GPS timeout path produces the `no_fix` golden-vector shape
