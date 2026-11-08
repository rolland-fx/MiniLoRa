# BLE Beacon Specification

## Standard
Bluetooth 5.0 — **Extended Advertising on Coded PHY** (LE PHY CODED, long-range ~100m+)

## Behavior
- Starts **after** sensor data collection and payload building, **before** LoRaWAN transmission.
- Runs continuously throughout the LoRa/WiFi cycle until deep sleep.
- Restarts every wake cycle (timer or button wake).

## Advertising Parameters

| Parameter | Value |
|-----------|-------|
| PHY | BLE_HCI_LE_PHY_CODED (both advertising and scanning) |
| Advertising interval | 16000 units × 0.625ms = ~1000ms |
| Extended advertising | Instance 0, Coded PHY |

## Payload
The advertising data carries the **uplink payload** (28 bytes) as manufacturer-specific data. This is the same payload sent via LoRaWAN/WiFi, allowing any BLE scanner to read the telemetry without connecting.

## Name
`GLOBAL_NAME` from `config_sensitive.h` (default: `"MiniLoRa-FX-1"`)

## Use Case
Passive telemetry broadcast — Android app does **not** need to connect; it can scan for manufacturer data in BLE advertisements to read live sensor data.

---

# BLE Config Server Specification

## Standard
Bluetooth 4.0+ — **GATT Server on Legacy 1M PHY** (connection-oriented, standard BLE)

## Behavior
- Activated only when GPIO_NUM_7 is **long-pressed** (≥ 3 seconds) during EXT1 wakeup.
- Runs for **120 seconds** waiting for a client to connect and write the config payload.
- Blue LED blinks every 500ms while waiting.
- If a write is received, saves config to NVS and goes to deep sleep.
- If timeout (120s) expires without write, goes to deep sleep without saving.

## Advertising Parameters

| Parameter | Value |
|-----------|-------|
| PHY | BLE_HCI_LE_PHY_1M (legacy, compatible with all phones) |
| Advertising interval | 16000 units × 0.625ms = ~1000ms |
| Legacy advertising | true (standard BLE discoverable) |
| Scannable | true |
| Connectable | Yes (GATT connection) |
| Flags | General Discoverable |

## GATT Profile

### Service
- **UUID**: `0000FACE-0000-1000-8000-00805F9B34FB`
- Type: Custom service

### Characteristic
- **UUID**: `0000CAFE-0000-1000-8000-00805F9B34FB`
- **Properties**: Read, Write (with response), Read encryption, Write encryption
- **Format**: Packed binary, 158 bytes, little-endian

### Payload Layout (158 bytes)

| Offset | Size | Field | Type | Description |
|--------|------|-------|------|-------------|
| 0 | 32 | `globalName` | UTF-8 null-padded | Device BLE/WiFi name |
| 32 | 32 | `wifiSSID` | UTF-8 null-padded | WiFi network name |
| 64 | 32 | `wifiPassword` | UTF-8 null-padded | WiFi password |
| 96 | 4 | `wifiServerIP` | uint32 LE | Server IP (e.g. `0x0A01A8C0` = 192.168.1.10) |
| 100 | 2 | `wifiServerPort` | uint16 LE | Server TCP port |
| 102 | 8 | `joinEUI` | uint64 LE | LoRaWAN Join EUI |
| 110 | 8 | `devEUI` | uint64 LE | LoRaWAN Device EUI |
| 118 | 16 | `appKey` | byte[16] | LoRaWAN Application Key |
| 134 | 16 | `nwkKey` | byte[16] | LoRaWAN Network Key |
| 150 | 1 | `cfgLoRaWAN` | uint8 bool | Enable LoRaWAN |
| 151 | 1 | `cfgWiFi` | uint8 bool | Enable WiFi HTTP |
| 152 | 1 | `cfgGNSS` | uint8 bool | Enable GPS |
| 153 | 1 | `cfgBME280` | uint8 bool | Enable BME280 |
| 154 | 1 | `cfgOPT30` | uint8 bool | Enable OPT3001 |
| 155 | 1 | `cfgLIS3DH` | uint8 bool | Enable LIS3DH |
| 156 | 1 | `cfgBLE` | uint8 bool | Enable BLE beacon |
| 157 | 1 | `cfgRange` | uint8 (0-8) | Sleep index (0=1min … 8=120min) |

## Protocol
1. Scan for device named `GLOBAL_NAME` (or whatever was set).
2. Connect and discover services.
3. Find service UUID `0000FACE-...` and characteristic UUID `0000CAFE-...`.
4. **Read** the characteristic to get the current config.
5. **Write** a complete 158-byte payload to update the config.
6. Device will stop advertising and go to sleep ~100ms after the write.

## Boolean Encoding
- `0x00` = false / disabled
- `0x01` = true / enabled

## String Encoding
Fixed-length 32 bytes, null-padded. Strings shorter than 32 must have null terminator at end. Unused bytes should be `0x00`.

## Discovery Notes
- The config server uses **legacy 1M PHY** advertising (standard BLE) — compatible with **all phones**, no BLE 5.0 extended advertising required.
- Scan by device name, not by service UUID.
