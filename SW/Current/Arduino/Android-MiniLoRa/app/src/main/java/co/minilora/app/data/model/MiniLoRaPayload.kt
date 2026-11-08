package co.minilora.app.data.model


import java.util.UUID

/**
 * Diagnostics uplink, schema 0x03 (24 bytes, fPort 2) — see SW/Current/ICD.md §2.
 * Byte 0 is the schema byte; decoders must switch on it.
 */
data class MiniLoRaDiag(
    val schema: Int,
    val fwVersion: String,
    val bootCount: Int,
    val unexpectedResets: Int,
    val lastResetReason: Int,
    val gpsTimeouts: Int,
    val loraJoinFailures: Int,
    val loraSendFailures: Int,
    val wifiFailures: Int,
    val zigbeeFailures: Int,
    val sensorReadFailures: Int,
    val gpsConsecFails: Int,
    val powerFlags: Int,
    val flags2: Int,
    // diag v4 (schema 0x04) — null for legacy 0x03 frames
    val lastTtff: Int?,
    val linkMargin: Int?,
    val gwCnt: Int?,
    val otaStatus: Int?,
    // diag v5 (schema 0x05) — null for pre-0x05 frames.
    // Transport/sensor enables (kCfgFlag* bits); LoRaWAN is always on.
    val configFlags: Int?,
) {
    val degraded: Boolean get() = powerFlags and 0x01 != 0
    val critical: Boolean get() = powerFlags and 0x02 != 0
    val gpsValid: Boolean get() = flags2 and 0x01 != 0
    val otaPending: Boolean get() = flags2 and 0x02 != 0
    val recoveryMode: Boolean get() = flags2 and 0x04 != 0

    companion object {
        val SIZES = mapOf(0x03 to 24, 0x04 to 28, 0x05 to 29) // schema -> frame length

        // Diag byte 28 configFlags — mirrors MiniLoRaCommands.CFG_FLAG_* ids.
        const val CFG_WIFI = 0x01
        const val CFG_ZIGBEE = 0x02
        const val CFG_BLE = 0x04
        const val CFG_GNSS = 0x08
        const val CFG_BME280 = 0x10
        const val CFG_OPT3001 = 0x20
        const val CFG_LIS3DH = 0x40

        fun fromBytes(b: ByteArray): MiniLoRaDiag? {
            val schema = b[0].toInt() and 0xFF
            val size = SIZES[schema] ?: return null
            if (b.size != size) return null
            fun u16(o: Int) = (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)
            fun u8orNull(o: Int) = if (schema >= 4) {
                val v = b[o].toInt() and 0xFF; if (v == 255) null else v
            } else null
            return MiniLoRaDiag(
                schema = schema,
                fwVersion = "${b[1]}.${b[2]}.${b[3]}",
                bootCount = u16(4),
                unexpectedResets = u16(6),
                lastResetReason = b[8].toInt() and 0xFF,
                gpsTimeouts = u16(9),
                loraJoinFailures = u16(11),
                loraSendFailures = u16(13),
                wifiFailures = u16(15),
                zigbeeFailures = u16(17),
                sensorReadFailures = u16(19),
                gpsConsecFails = b[21].toInt() and 0xFF,
                powerFlags = b[22].toInt() and 0xFF,
                flags2 = b[23].toInt() and 0xFF,
                lastTtff = u8orNull(24),
                linkMargin = u8orNull(25),
                gwCnt = u8orNull(26),
                otaStatus = u8orNull(27),
                configFlags = if (schema >= 5) b[28].toInt() and 0xFF else null,
            )
        }
    }

    /** True when the flag is known-set; false when known-clear; null when the
     *  diag frame predates schema 0x05 (state unknown). */
    fun cfgFlag(mask: Int): Boolean? = configFlags?.let { it and mask != 0 }
}

/** fPort-3 remote command opcodes (ICD §3). */
object MiniLoRaCommands {
    const val MAGIC = 0x4D
    const val LOCATE_NOW = 0x01
    const val SEND_DIAG = 0x02
    const val RESET_DIAG = 0x03
    const val BLE_WINDOW = 0x04
    const val OTA_UPDATE = 0x05
    const val CFG_SET = 0x06 // ICD 3.5 §3: arg bit7 = value, bits0-3 = flag id
    const val SET_RANGE = 0x07 // ICD 3.6 §3: arg = index into the sleep table

    // CFG_SET flag ids. LoRaWAN is deliberately not addressable.
    const val CFG_WIFI = 1
    const val CFG_ZIGBEE = 2
    const val CFG_BLE = 3     // the telemetry beacon (config is always available)
    const val CFG_GNSS = 4
    const val CFG_BME280 = 5
    const val CFG_OPT3001 = 6
    const val CFG_LIS3DH = 7

    fun frame(opcode: Int, arg: Int = 0): ByteArray =
        byteArrayOf(MAGIC.toByte(), opcode.toByte(), arg.toByte())

    /** CFG_SET frame: arg = (value shl 7) or flagId. */
    fun configSet(flagId: Int, on: Boolean): ByteArray =
        frame(CFG_SET, ((if (on) 1 else 0) shl 7) or flagId)

    /** Sleep-table labels (index 0..8) — mirrors the firmware
     *  kSleepTableMs; shared by the config tab and the remote tab. */
    val SLEEP_LABELS = listOf(
        "1 min", "2 min", "5 min", "10 min", "15 min", "20 min",
        "30 min", "60 min", "120 min",
    )

    /** SET_RANGE frame: arg = the sleep-table index (same scale as the
     *  config envelope's cfgRange). Confirm rides the next telemetry
     *  frame's sleep-minutes byte. */
    fun setRange(index: Int): ByteArray = frame(SET_RANGE, index)

    val NAMES = mapOf(LOCATE_NOW to "locateNow", SEND_DIAG to "sendDiag",
                      RESET_DIAG to "resetDiag", BLE_WINDOW to "bleWindow",
                      OTA_UPDATE to "otaUpdate", CFG_SET to "configSet",
                      SET_RANGE to "setRange")
}


/**
 * Decoded MiniLoRa 28-byte payload from a BLE scan record.
 *
 * Payload specification (little-endian encoding):
 *   Byte 0:  Temp        int8_t       Direct °C value
 *   Byte 1:  Hum         uint8_t      % relative humidity
 *   Byte 2-3: Pressure   uint16_t     hPa
 *   Byte 4:  Bat_vol     uint8_t      0→2500mV, 255→4300mV (linear)
 *   Byte 5-6: Luminosity uint16_t     Lux
 *   Byte 7:  Acc_x       uint8_t      -10000→0, 10000→255 (128 = 0)
 *   Byte 8:  Acc_y       uint8_t      same mapping
 *   Byte 9:  Acc_z       uint8_t      same mapping
 *   Byte 10-13: Latitude int32_t      Microdegrees (×1,000,000)
 *   Byte 14-17: Longitude int32_t     Microdegrees (×1,000,000)
 *   Byte 18-19: Altitude int16_t      Meters + 1000 offset
 *   Byte 20: Satellites  uint8_t      Satellite count
 *   Byte 21-24: Time      uint32_t    Unix epoch seconds
 *   Byte 25: AwakeSec    uint8_t      Seconds since device woke
 *   Byte 26: SleepMinutes uint8_t     Deep sleep duration in minutes
 *   Byte 27: GPIOStatus  uint8_t      Bitmask: bit0=LED_GREEN, bit1=Button_2
 */
class MiniLoRaPayload(rawBytes: ByteArray) {

    companion object {
        const val PAYLOAD_SIZE = 28
        val SERVICE_UUID = UUID.fromString("0000fee0-0000-1000-8000-00805f9b34fb")
    }

    private val data: ByteArray = rawBytes.copyOf(PAYLOAD_SIZE)

    init {
        require(data.size >= PAYLOAD_SIZE) {
            "MiniLoRa payload must be at least $PAYLOAD_SIZE bytes, got ${data.size}"
        }
    }

    val temperatureCelsius: Float
        get() = data[0].toFloat()

    val humidityPercent: Int
        get() = data[1].toInt() and 0xFF

    /**
     * Atmospheric pressure in hPa.
     * Payload bytes 2-3 carry 0.1 hPa units (deci-hPa): 10130 -> 1013.0 hPa.
     */
    val pressureHpa: Float
        get() = readUint16(data, 2) / 10f

    /**
     * Battery voltage in millivolts.
     * Mapping: 0 → 2500mV, 255 → 4300mV (linear)
     * Formula: raw * (1800 / 255) + 2500
     */
    val batteryVoltageMV: Int
        get() {
            val raw = data[4].toInt() and 0xFF
            return (raw * 1800 / 255) + 2500
        }

    val batteryVoltage: Float
        get() = batteryVoltageMV / 1000f

    val luminosityLux: Int
        get() = readUint16(data, 5)

    /**
     * Accelerometer X axis, in milli-g.
     * The payload byte carries LIS3DH 10-bit counts on the v1 ±10000 map
     * (128 = 0); the sensor runs at ±4 g, where 1 g = 512 counts — so the
     * counts convert to mg at ×4000/2048. Flat on a table this reads
     * Z ≈ +1000 mg, X/Y ≈ 0 (the old /1000-as-g display read Z ≈ 0.43).
     */
    val accX: Double
        get() = mapAccel(data[7].toInt() and 0xFF) * MG_PER_COUNT

    val accY: Double
        get() = mapAccel(data[8].toInt() and 0xFF) * MG_PER_COUNT

    val accZ: Double
        get() = mapAccel(data[9].toInt() and 0xFF) * MG_PER_COUNT

    val latitude: Double
        get() = readInt32(data, 10) / 1_000_000.0

    val longitude: Double
        get() = readInt32(data, 14) / 1_000_000.0

    val altitude: Int
        get() = readInt16(data, 18) - 1000

    val satellites: Int
        get() = data[20].toInt() and 0xFF

    val timeUnixSeconds: Long
        get() = readUint32(data, 21).toLong()

    val date: java.util.Date
        get() = java.util.Date(timeUnixSeconds * 1000)

    val awakeSeconds: Int
        get() = data[25].toInt() and 0xFF

    val sleepMinutes: Int
        get() = data[26].toInt() and 0xFF

    val gpioStatus: Int
        get() = data[27].toInt() and 0xFF

    val isLedGreenOn: Boolean
        get() = gpioStatus and 0x01 != 0

    val isButton2Pressed: Boolean
        get() = gpioStatus and 0x02 != 0

    val rawBytesData: ByteArray
        get() = data.copyOf(PAYLOAD_SIZE)

    /** Raw 28-byte array for debugging / logging */
    val rawBytes: ByteArray
        get() = data

    private fun readUint16(data: ByteArray, offset: Int): Int {
        return (data[offset].toInt() and 0xFF) or
               (data[offset + 1].toInt() and 0xFF shl 8)
    }

    private fun readInt16(data: ByteArray, offset: Int): Int {
        val raw = readUint16(data, offset).toLong()
        return if (raw and 0x8000 != 0L) (raw or 0xFFFF0000).toInt() else raw.toInt()
    }

    private fun readInt32(data: ByteArray, offset: Int): Int {
        return (data[offset].toInt() and 0xFF) or
               (data[offset + 1].toInt() and 0xFF shl 8) or
               (data[offset + 2].toInt() and 0xFF shl 16) or
               (data[offset + 3].toInt() and 0xFF shl 24)
    }

    private fun readUint32(data: ByteArray, offset: Int): Int {
        return (data[offset].toInt() and 0xFF) or
               (data[offset + 1].toInt() and 0xFF shl 8) or
               (data[offset + 2].toInt() and 0xFF shl 16) or
               (data[offset + 3].toInt() and 0xFF shl 24)
    }

    private fun mapAccel(value: Int): Double {
        // v1 ABI: 0 → -10000, 128 → 0, 255 → +10000 — the map carries
        // LIS3DH counts, not mg (see the accessors above).
        return (value - 127) * 10000.0 / 127.0
    }
}

/** counts → mg at the payload's ±4 g full scale (MiniLoRaPayload.kt). */
const val MG_PER_COUNT = 4000.0 / 2048.0

/**
 * Helper object for parsing MiniLoRa 28-byte payloads from BLE scan data.
 * Parses raw advertising data bytes to extract the payload from
 * Service Data (AD type 0x16, UUID 0xFEE0) or Manufacturer Specific Data (AD type 0xFF).
 */
object MiniLoRaPayloadParser {

    fun fromBytes(data: ByteArray?): MiniLoRaPayload? {
        if (data == null || data.isEmpty()) return null
        val payload = extractPayload(data)
        return if (payload != null && payload.size >= MiniLoRaPayload.PAYLOAD_SIZE) {
            MiniLoRaPayload(payload)
        } else {
            null
        }
    }

    fun fromScanRecord(scanRecord: android.bluetooth.le.ScanRecord?): MiniLoRaPayload? {
        if (scanRecord == null) return null
        val bytes = try {
            scanRecord.getBytes()
        } catch (e: Exception) {
            return null
        }
        return fromBytes(bytes)
    }

    fun fromScanResult(scanResult: android.bluetooth.le.ScanResult): MiniLoRaPayload? {
        try {
            val bytes = scanResult.scanRecord?.getBytes()
            return fromBytes(bytes)
        } catch (e: Exception) {
            return null
        }
    }

    /** Convert bytes to hex string for debugging */
    private fun bytesToHex(bytes: ByteArray): String {
        return bytes.joinToString(" ") { "%02X".format(it) }
    }

    private fun extractPayload(adData: ByteArray): ByteArray? {
        var offset = 0

        while (offset < adData.size) {
            if (offset + 2 > adData.size) break
            val len = adData[offset].toInt() and 0xFF
            val type = adData[offset + 1].toInt() and 0xFF

            if (len <= 0 || offset + 1 + len > adData.size) break

            val fieldData = adData.copyOfRange(offset + 2, offset + len + 1)

            when (type) {
                0xFF -> {
                    // Manufacturer Specific Data
                    if (fieldData.size == 28) {
                        return fieldData.copyOf(28)
                    } else if (fieldData.size >= 30) {
                        // 2 manufacturer ID bytes + 28 data bytes
                        val payload = fieldData.copyOfRange(2, 30)
                        if (payload.size == 28) {
                            return payload
                        }
                    }
                }
                0x16 -> {
                    // Service Data (AD type 0x16) with 16-bit UUID 0xFEE0/FEE3/F3FE
                    if (fieldData.size >= 5) {
                        val uuidLSB = fieldData[0].toInt() and 0xFF
                        val uuidMSB = fieldData[1].toInt() and 0xFF
                        val uuidLE = uuidLSB or (uuidMSB shl 8)
                        val supportedUuids = setOf(0xFEE0, 0xFEE3, 0xF3FE)
                        if (uuidLE in supportedUuids) {
                            // fieldData[0..1] = UUID, rest = payload (may be 27-28 bytes)
                            val availablePayloadSize = fieldData.size - 2
                            val payloadSize = minOf(availablePayloadSize, 28)
                            val payload = fieldData.copyOfRange(2, 2 + payloadSize)
                            if (payload.size == 28) {
                                android.util.Log.d("MiniLoRa", "ServiceData UUID=${String.format("%04X", uuidLE)} payload=${payload.size}B at offset=$offset")
                                android.util.Log.d("MiniLoRa", "  payload=${payload.take(10).joinToString(" ") { "%02X".format(it) }}")
                                return payload
                            }
                            // Accept 27-byte payload - try padding with 0x00 as last byte
                            if (payload.size == 27 && availablePayloadSize >= 27) {
                                val paddedPayload = payload + byteArrayOf(0x00)
                                android.util.Log.d("MiniLoRa", "ServiceData UUID=${String.format("%04X", uuidLE)} payload=${payload.size}B padded to 28")
                                return paddedPayload
                            }
                            android.util.Log.d("MiniLoRa", "ServiceData UUID=${String.format("%04X", uuidLE)} payload=${payload.size}B (need 28)")
                        } else {
                            android.util.Log.d("MiniLoRa", "ServiceData UUID=0x${String.format("%04X", uuidLE)} NOT supported")
                        }
                    }
                }
            }

            offset += 1 + len
        }

        return null
    }

}