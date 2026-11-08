package co.minilora.app.ble

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import java.util.Arrays

/**
 * Manages BLE GATT connection to a MiniLoRa device for reading/writing the configuration
 * characteristic defined in SW/Current/ICD.md (§4 write, §5 status).
 *
 * Service:        0000FACE-0000-1000-8000-00805F9B34FB
 * Characteristic: 0000CAFE-0000-1000-8000-00805F9B34FB  (config, read/write)
 * Status char:    0000CAFF-0000-1000-8000-00805F9B34FB  (status, read-only, 16 B)
 */
class BLEDeviceConfigurator(private val context: Context) {

    companion object {
    // ---- ICD v3 (SW/Current/ICD.md) -------------------------------------

    /** CRC16-CCITT (poly 0x1021, init 0xFFFF) — the ICD §4 checksum. */
    fun crc16Ccitt(data: ByteArray, start: Int = 0, len: Int = data.size - start): Int {
        var crc = 0xFFFF
        for (i in start until start + len) {
            crc = crc xor ((data[i].toInt() and 0xFF) shl 8)
            repeat(8) {
                crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xFFFF
                      else (crc shl 1) and 0xFFFF
            }
        }
        return crc
    }

    /**
     * Build the 164-byte config body (ICD §4 schema 4): the v1 field
     * layout + cfgZigbee + the 5 asset bytes. Strings null-padded,
     * multi-byte LE — except the IP (octet order, see note).
     */
    fun buildBody(
        globalName: String, wifiSSID: String, wifiPassword: String,
        wifiServerIP: Long, wifiServerPort: Int, joinEUI: Long, devEUI: Long,
        appKey: ByteArray, nwkKey: ByteArray, cfgLoRaWAN: Boolean,
        cfgWiFi: Boolean, cfgGNSS: Boolean, cfgBME280: Boolean,
        cfgOPT30: Boolean, cfgLIS3DH: Boolean, cfgBLE: Boolean,
        cfgZigbee: Boolean, cfgRange: Int,
        cfgAssetMode: Boolean, cfgMotionSens: Int,
        cfgAssetHeartbeatH: Int, cfgBurstIntervalMin: Int,
        cfgBurstMaxPerDay: Int
    ): ByteArray {
        val buf = ByteArray(CONFIG_BODY_V4)

        // Strings (32 bytes each, null-padded, UTF-8)
        globalName.padEnd(32, '\u0000').toByteArray(Charsets.UTF_8).copyInto(buf, 0)
        wifiSSID.padEnd(32, '\u0000').toByteArray(Charsets.UTF_8).copyInto(buf, 32)
        wifiPassword.padEnd(32, '\u0000').toByteArray(Charsets.UTF_8).copyInto(buf, 64)

        // Numeric fields. IP convention: parseIP() packs octets MSB-first
        // (192.168.1.10 → 0xC0A80228) and the write is BIG_ENDIAN, so the
        // wire bytes are C0 A8 01 0A — matching the firmware's "o0 = LSB
        // octet" read.
        ByteBuffer.wrap(buf, 96, 4).order(ByteOrder.BIG_ENDIAN).putInt(wifiServerIP.toInt())
        ByteBuffer.wrap(buf, 100, 2).order(ByteOrder.LITTLE_ENDIAN).putShort(wifiServerPort.toShort())
        ByteBuffer.wrap(buf, 102, 8).order(ByteOrder.LITTLE_ENDIAN).putLong(joinEUI)
        ByteBuffer.wrap(buf, 110, 8).order(ByteOrder.LITTLE_ENDIAN).putLong(devEUI)

        // Keys
        appKey.copyInto(buf, 118)
        nwkKey.copyInto(buf, 134)

        // Booleans, range, Zigbee
        buf[150] = if (cfgLoRaWAN) 0x01 else 0x00
        buf[151] = if (cfgWiFi)     0x01 else 0x00
        buf[152] = if (cfgGNSS)     0x01 else 0x00
        buf[153] = if (cfgBME280)   0x01 else 0x00
        buf[154] = if (cfgOPT30)    0x01 else 0x00
        buf[155] = if (cfgLIS3DH)   0x01 else 0x00
        buf[156] = if (cfgBLE)      0x01 else 0x00
        buf[157] = cfgRange.toByte()
        buf[158] = if (cfgZigbee)   0x01 else 0x00
        buf[159] = if (cfgAssetMode) 0x01 else 0x00
        buf[160] = cfgMotionSens.coerceIn(0, 3).toByte()
        buf[161] = cfgAssetHeartbeatH.coerceIn(0, 255).toByte()
        buf[162] = cfgBurstIntervalMin.coerceIn(0, 255).toByte()
        buf[163] = cfgBurstMaxPerDay.coerceIn(0, 255).toByte()

        return buf
    }

    /**
     * Build the 168-byte ICD §4 write envelope (schema 4):
     * `'C' | 4 | 164-byte body | CRC16-CCITT(body) LE`.
     * The firmware rejects the write if the CRC does not match.
     */
    fun buildEnvelope(body: ByteArray): ByteArray {
        require(body.size == CONFIG_BODY_V4) { "body must be $CONFIG_BODY_V4 bytes" }
        val env = ByteArray(CONFIG_ENVELOPE_V4)
        env[0] = 'C'.code.toByte()
        env[1] = 4
        body.copyInto(env, 2)
        val crc = crc16Ccitt(body)
        env[CONFIG_ENVELOPE_V4 - 2] = (crc and 0xFF).toByte()
        env[CONFIG_ENVELOPE_V4 - 1] = ((crc shr 8) and 0xFF).toByte()
        return env
    }


        private val SERVICE_UUID = UUID.fromString("0000FACE-0000-1000-8000-00805F9B34FB")
        private val CHAR_UUID    = UUID.fromString("0000CAFE-0000-1000-8000-00805F9B34FB")
        private val STATUS_UUID  = UUID.fromString("0000CAFF-0000-1000-8000-00805F9B34FB")
        const val CONFIG_BODY_V3 = 159 // v1 layout + cfgZigbee
        const val CONFIG_BODY_V4 = 164 // + 5 asset bytes (P5)
        const val CONFIG_ENVELOPE_V3 = 163 // 'C'|3|159|CRC
        const val CONFIG_ENVELOPE_V4 = 168 // 'C'|4|164|CRC  — current
        private const val TAG = "BLEDeviceConfigurator"
    }

    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private var statusCharacteristic: BluetoothGattCharacteristic? = null
    var onReadComplete: ((ByteArray) -> Unit)? = null
    var onWriteComplete: ((Boolean) -> Unit)? = null
    var onStatusRead: ((ByteArray) -> Unit)? = null
    var onConnectionState: ((Boolean) -> Unit)? = null

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "GATT connection error: $status")
                onConnectionState?.invoke(false)
                return
            }
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.d(TAG, "Connected, starting service discovery")
                gatt.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.d(TAG, "Disconnected")
                onConnectionState?.invoke(false)
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "Service discovery failed: $status")
                onConnectionState?.invoke(false)
                return
            }
            val svc = gatt.getService(SERVICE_UUID)
            if (svc == null) {
                Log.e(TAG, "Config service not found")
                onConnectionState?.invoke(false)
                return
            }
            characteristic = svc.getCharacteristic(CHAR_UUID)
            statusCharacteristic = svc.getCharacteristic(STATUS_UUID)
            if (characteristic == null) {
                Log.e(TAG, "Config characteristic not found")
                onConnectionState?.invoke(false)
                return
            }
            Log.d(TAG, "Services discovered, ready (status char: ${statusCharacteristic != null})")
            onConnectionState?.invoke(true)
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "Read failed: $status")
                onReadComplete?.invoke(byteArrayOf())
                return
            }
            val data = characteristic.value ?: byteArrayOf()
            Log.d(TAG, "Read ${data.size} bytes (uuid=${characteristic.uuid})")
            if (characteristic.uuid == STATUS_UUID) {
                onStatusRead?.invoke(data)
            } else {
                onReadComplete?.invoke(data)
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            val ok = status == BluetoothGatt.GATT_SUCCESS
            Log.d(TAG, "Write status=$status ok=$ok")
            onWriteComplete?.invoke(ok)
            // Disconnect after write
            disconnect()
        }
    }

    /** Connect to a BLE device and discover the config service */
    fun connect(device: BluetoothDevice) {
        Log.d(TAG, "Connecting to ${device.address} (LE transport)")
        // TRANSPORT_LE is essential: without it Android may attempt a
        // BR/EDR (classic) connection to this BLE-only tracker and sit
        // ~30 s in silence before failing ("connection failed").
        gatt = device.connectGatt(
            context, false, callback, BluetoothDevice.TRANSPORT_LE,
        )
    }

    /** Read the config characteristic (triggers device to respond with 158-byte payload) */
    fun readConfig() {
        val ch = characteristic ?: run {
            Log.e(TAG, "No characteristic discovered yet")
            onReadComplete?.invoke(byteArrayOf())
            return
        }
        Log.d(TAG, "Requesting read")
        gatt?.readCharacteristic(ch)
    }

    /** Read the status characteristic (16 bytes, ICD §5 — fresh battery, counters, joined). */
    fun readStatus() {
        val ch = statusCharacteristic ?: run {
            Log.w(TAG, "Status characteristic not available (old firmware?)")
            onStatusRead?.invoke(byteArrayOf())
            return
        }
        gatt?.readCharacteristic(ch)
    }

    /** Write a config envelope; schema 3 (163 B) and 4 (168 B) accepted. */
    fun writeConfig(payload: ByteArray) {
        val ch = characteristic ?: run {
            Log.e(TAG, "No characteristic discovered yet")
            onWriteComplete?.invoke(false)
            return
        }
        if (payload.size != CONFIG_ENVELOPE_V3 &&
            payload.size != CONFIG_ENVELOPE_V4) {
            Log.e(TAG, "Payload must be $CONFIG_ENVELOPE_V3 or $CONFIG_ENVELOPE_V4 bytes, got ${payload.size}")
            onWriteComplete?.invoke(false)
            return
        }
        Log.d(TAG, "Writing ${payload.size} bytes")
        ch.value = payload
        gatt?.writeCharacteristic(ch)
    }

    /** Disconnect and clean up */
    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        characteristic = null
    }



    /**
     * Parse a config read: a schema-3 (163 B) or schema-4 (168 B) envelope,
     * or the raw body (159/164). Legacy 158-byte blobs are not accepted.
     * Asset fields are absent (null) from schema-3 bodies.
     */
    fun parsePayload(data: ByteArray): Map<String, Any> {
        var body = data
        var assetFields = false
        if (data.size == CONFIG_ENVELOPE_V4 && data[0] == 'C'.code.toByte() &&
            data[1].toInt() == 4) {
            if (crc16Ccitt(data, 2, CONFIG_BODY_V4) !=
                ((data[CONFIG_ENVELOPE_V4 - 2].toInt() and 0xFF) or
                 ((data[CONFIG_ENVELOPE_V4 - 1].toInt() and 0xFF) shl 8))) {
                Log.e(TAG, "Config read CRC mismatch")
                return emptyMap()
            }
            body = data.copyOfRange(2, 2 + CONFIG_BODY_V4)
            assetFields = true
        } else if (data.size == CONFIG_BODY_V4) {
            assetFields = true
        } else if (data.size == CONFIG_ENVELOPE_V3 && data[0] == 'C'.code.toByte() &&
                   data[1].toInt() == 3) {
            if (crc16Ccitt(data, 2, CONFIG_BODY_V3) !=
                ((data[CONFIG_ENVELOPE_V3 - 2].toInt() and 0xFF) or
                 ((data[CONFIG_ENVELOPE_V3 - 1].toInt() and 0xFF) shl 8))) {
                Log.e(TAG, "Config read CRC mismatch")
                return emptyMap()
            }
            body = data.copyOfRange(2, 2 + CONFIG_BODY_V3)
        } else if (data.size != CONFIG_BODY_V3) {
            Log.e(TAG, "Bad config size: ${data.size}")
            return emptyMap()
        }
        val result = mutableMapOf<String, Any>()
        // Strings
        result["globalName"] = body.sliceArray(0..31).takeWhile { it != 0.toByte() }.toByteArray().decodeToString()
        result["wifiSSID"]   = body.sliceArray(32..63).takeWhile { it != 0.toByte() }.toByteArray().decodeToString()
        result["wifiPassword"] = body.sliceArray(64..95).takeWhile { it != 0.toByte() }.toByteArray().decodeToString()
        // Numeric — IP: parseIP() packs MSB-first and the blob stores it
        // BIG_ENDIAN (wire order = octet order C0 A8 01 0A for 192.168.1.10).
        val ipBuf = ByteBuffer.wrap(body, 96, 4).order(ByteOrder.BIG_ENDIAN)
        result["wifiServerIP"] = ipBuf.int.toLong() and 0xFFFFFFFFL
        val portBuf = ByteBuffer.wrap(body, 100, 2).order(ByteOrder.LITTLE_ENDIAN)
        result["wifiServerPort"] = portBuf.short.toInt() and 0xFFFF
        val joinBuf = ByteBuffer.wrap(body, 102, 8).order(ByteOrder.LITTLE_ENDIAN)
        result["joinEUI"] = joinBuf.long
        val devBuf = ByteBuffer.wrap(body, 110, 8).order(ByteOrder.LITTLE_ENDIAN)
        result["devEUI"] = devBuf.long
        // Keys as hex strings
        result["appKey"] = body.sliceArray(118..133).joinToString("") { "%02x".format(it) }
        result["nwkKey"] = body.sliceArray(134..149).joinToString("") { "%02x".format(it) }
        // Booleans (NOTE: must index `body`, not `data` — `data` is the
        // 168-byte envelope, so data[150..157] are 2 bytes early and land
        // in the tail of nwkKey; bench 2026-11-01: cfgRange came back as
        // cfgLIS3DH and every flag was shifted into the keys).
        result["cfgLoRaWAN"] = body[150] == 0x01.toByte()
        result["cfgWiFi"]    = body[151] == 0x01.toByte()
        result["cfgGNSS"]    = body[152] == 0x01.toByte()
        result["cfgBME280"]  = body[153] == 0x01.toByte()
        result["cfgOPT30"]   = body[154] == 0x01.toByte()
        result["cfgLIS3DH"]  = body[155] == 0x01.toByte()
        result["cfgBLE"]     = body[156] == 0x01.toByte()
        result["cfgRange"]   = body[157].toInt() and 0xFF
        result["cfgZigbee"] = body[158] == 0x01.toByte()
        if (assetFields) {
            result["cfgAssetMode"] = body[159] == 0x01.toByte()
            result["cfgMotionSens"] = body[160].toInt() and 0xFF
            result["cfgAssetHeartbeatH"] = body[161].toInt() and 0xFF
            result["cfgBurstIntervalMin"] = body[162].toInt() and 0xFF
            result["cfgBurstMaxPerDay"] = body[163].toInt() and 0xFF
        }
        return result
    }

    /** Parse the 16-byte status frame (ICD §5) into a readable map. */
    fun parseStatus(data: ByteArray): Map<String, Any> {
        if (data.size < 16 || data[0].toInt() != 0x01) {
            Log.e(TAG, "Bad status frame (${data.size} bytes)")
            return emptyMap()
        }
        val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        val flags2 = data[13].toInt() and 0xFF
        return mapOf(
            "schema" to 1,
            "fwVersion" to "${data[1]}.${data[2]}.${data[3]}",
            "batMv" to (buf.getShort(4).toInt() and 0xFFFF),
            "bootCount" to (buf.getShort(6).toInt() and 0xFFFF),
            "unexpectedResets" to (buf.getShort(8).toInt() and 0xFFFF),
            "lastResetReason" to (data[10].toInt() and 0xFF),
            "gpsConsecFails" to (data[11].toInt() and 0xFF),
            "powerFlags" to (data[12].toInt() and 0xFF),
            "gpsValid" to (flags2 and 0x01 != 0),
            "otaPending" to (flags2 and 0x02 != 0),
            "recoveryMode" to (flags2 and 0x04 != 0),
            "joined" to (data[14].toInt() != 0),
        )
    }
}
