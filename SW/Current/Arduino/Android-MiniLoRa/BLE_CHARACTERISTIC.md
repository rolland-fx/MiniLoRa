# BLE Configuration Characteristic Specification

> **Superseded:** the canonical (and now envelope-only) contract is
> [`../../ICD.md`](../../ICD.md) §4–5. This document describes the *field
> layout*, which is unchanged inside the envelope body.

## Service UUID
`0000FACE-0000-1000-8000-00805F9B34FB`

## Characteristic UUID
`0000CAFE-0000-1000-8000-00805F9B34FB`

## Access
Read / Write (with response)

## Advertising Standard
Bluetooth 4.0+ **Legacy 1M PHY** — the config server uses legacy advertising on 1M PHY (standard BLE, compatible with all phones). Phones do NOT need BLE 5.0 extended advertising support.

## Format
Packed binary payload, 158 bytes total. All multi-byte numeric fields are **little-endian** (ESP32 native).

## Payload Layout

| Offset | Size | Field | Type | Description |
|--------|------|-------|------|-------------|
| 0 | 32 | `globalName` | UTF-8 string, null-padded | Device name used for BLE advertising & WiFi hostname |
| 32 | 32 | `wifiSSID` | UTF-8 string, null-padded | WiFi network name |
| 64 | 32 | `wifiPassword` | UTF-8 string, null-padded | WiFi password |
| 96 | 4 | `wifiServerIP` | uint32 LE | Server IP (e.g. `0x0A01A8C0` = 192.168.1.10) |
| 100 | 2 | `wifiServerPort` | uint16 LE | Server TCP port |
| 102 | 8 | `joinEUI` | uint64 LE | LoRaWAN Join EUI |
| 110 | 8 | `devEUI` | uint64 LE | LoRaWAN Device EUI |
| 118 | 16 | `appKey` | byte[16] | LoRaWAN Application Key |
| 134 | 16 | `nwkKey` | byte[16] | LoRaWAN Network Key |
| 150 | 1 | `cfgLoRaWAN` | uint8 (bool) | Enable LoRaWAN telemetry |
| 151 | 1 | `cfgWiFi` | uint8 (bool) | Enable WiFi HTTP telemetry |
| 152 | 1 | `cfgGNSS` | uint8 (bool) | Enable GPS receiver |
| 153 | 1 | `cfgBME280` | uint8 (bool) | Enable BME280 temperature/humidity/pressure sensor |
| 154 | 1 | `cfgOPT30` | uint8 (bool) | Enable OPT3001 light sensor |
| 155 | 1 | `cfgLIS3DH` | uint8 (bool) | Enable LIS3DH accelerometer |
| 156 | 1 | `cfgBLE` | uint8 (bool) | Enable BLE beacon |
| 157 | 1 | `cfgRange` | uint8 (0-8) | Sleep duration index: 0=1min, 1=2min, 2=5min, 3=10min, 4=15min, 5=20min, 6=30min, 7=60min, 8=120min |

## Boolean Values
- `0x00` = false / disabled
- `0x01` = true / enabled

## String Fields
All string fields (`globalName`, `wifiSSID`, `wifiPassword`) are fixed-length 32 bytes, null-padded. Unused bytes should be `0x00`. The device reads exactly 32 bytes — strings shorter than 32 must be null-terminated.

## Example (Android — Kotlin)
```kotlin
val SERVICE_UUID  = UUID.fromString("0000FACE-0000-1000-8000-00805F9B34FB")
val CHAR_UUID     = UUID.fromString("0000CAFE-0000-1000-8000-00805F9B34FB")

// Write configuration
fun writeConfig(globalName: String, ssid: String, pass: String, ip: Long, port: Int,
                joinEui: Long, devEui: Long, appKey: ByteArray, nwkKey: ByteArray,
                lorawan: Boolean, wifi: Boolean, gnss: Boolean, bme280: Boolean,
                opt30: Boolean, lis3dh: Boolean, ble: Boolean, range: Int) {

    val buf = ByteArray(158)

    // Strings (32 bytes each, null-padded)
    globalName.padEnd(32, '\u0000').toByteArray(Charset.forName("UTF-8")).copyInto(buf, 0)
    ssid.padEnd(32, '\u0000').toByteArray(Charset.forName("UTF-8")).copyInto(buf, 32)
    pass.padEnd(32, '\u0000').toByteArray(Charset.forName("UTF-8")).copyInto(buf, 64)

    // Numeric fields — little-endian
    java.nio.ByteBuffer.wrap(buf, 96, 4).order(LITTLE_ENDIAN).putInt(ip.toInt())
    java.nio.ByteBuffer.wrap(buf, 100, 2).order(LITTLE_ENDIAN).putShort(port.toShort())
    java.nio.ByteBuffer.wrap(buf, 102, 8).order(LITTLE_ENDIAN).putLong(joinEui)
    java.nio.ByteBuffer.wrap(buf, 110, 8).order(LITTLE_ENDIAN).putLong(devEui)

    // Keys
    appKey.copyInto(buf, 118)
    nwkKey.copyInto(buf, 134)

    // Booleans & range
    buf[150] = if (lorawan) 0x01 else 0x00
    buf[151] = if (wifi)    0x01 else 0x00
    buf[152] = if (gnss)    0x01 else 0x00
    buf[153] = if (bme280)  0x01 else 0x00
    buf[154] = if (opt30)   0x01 else 0x00
    buf[155] = if (lis3dh)  0x01 else 0x00
    buf[156] = if (ble)     0x01 else 0x00
    buf[157] = range.toByte()

    // Write characteristic
    bleGatt.writeCharacteristic(SERVICE_UUID, CHAR_UUID, buf)
}

// Read configuration — parse the same buffer on read callback
```

## Reading Current Values
Use a standard BLE read request on the characteristic. The device responds with the current full 158-byte payload.
