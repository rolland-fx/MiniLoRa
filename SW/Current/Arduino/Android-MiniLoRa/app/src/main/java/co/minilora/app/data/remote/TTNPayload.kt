package co.minilora.app.data.remote

import android.util.Base64
import android.util.Log
import co.minilora.app.data.model.MiniLoRaDiag
import co.minilora.app.data.model.MiniLoRaPayload

/**
 * Decodes a TTN uplink message's data_raw field into a MiniLoRaPayload.
 *
 * TTN's data_raw is base64-encoded raw bytes.
 * It may also be hex-encoded in some deployments.
 */
fun decodeTTNPayload(dataRaw: String): MiniLoRaPayload? {
    val bytes = base64ToBytes(dataRaw)
        ?: hexStringToBytes(dataRaw)
        ?: return null
    return try {
        MiniLoRaPayload(bytes)
    } catch (e: Exception) {
        Log.e("TTNPayload", "Failed to build MiniLoRaPayload: ${e.message}")
        null
    }
}

/** Decodes a diagnostics frame (fPort 2, schema 0x03). Returns null if not one. */
fun decodeTTNDiag(dataRaw: String): MiniLoRaDiag? {
    val bytes = base64ToBytes(dataRaw) ?: hexStringToBytes(dataRaw) ?: return null
    return try {
        MiniLoRaDiag.fromBytes(bytes)
    } catch (e: Exception) {
        Log.e("TTNPayload", "Failed to build MiniLoRaDiag: ${e.message}")
        null
    }
}

internal fun base64ToBytes(base64: String): ByteArray? {
    return try {
        Base64.decode(base64, Base64.DEFAULT)
    } catch (e: Exception) {
        null
    }
}

private fun hexStringToBytes(hex: String): ByteArray? {
    val clean = hex.replace(" ", "").replace("\n", "")
    if (clean.length % 2 != 0) return null
    val len = clean.length / 2
    return try {
        ByteArray(len) { clean.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    } catch (_: NumberFormatException) {
        null
    }
}
