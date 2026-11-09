package co.minilora.app.data.local

import android.content.Context
import android.util.Base64
import android.util.Log
import co.minilora.app.data.model.MiniLoRaDiag
import org.json.JSONObject
import java.io.File

/**
 * Persists the latest diagnostics frame (raw bytes + when it was received)
 * so the Remote tab keeps showing it — across screen changes AND app
 * restarts — until a new one arrives. Mirrors PayloadStore's file-JSON
 * pattern; the frame is stored raw so the parsed model can be rebuilt with
 * MiniLoRaDiag.fromBytes, which stays the single decoder.
 *
 * Keyed per tracker: each device identity (BLE MAC, TTN id, WiFi name)
 * gets its own file, so a multi-tracker setup keeps one diag card per
 * tracker. The legacy single-device file name is kept for the empty key.
 */
object DiagStore {

    private const val TAG = "DiagStore"
    private const val FILE_NAME = "last_diag.json"

    /** last_diag_<sanitized-key>.json; "" keeps the legacy file name. */
    private fun fileFor(context: Context, deviceKey: String): File {
        val safe = deviceKey.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val name = if (deviceKey.isBlank()) FILE_NAME
                   else "last_diag_$safe.json"
        return File(context.filesDir, name)
    }

    fun save(context: Context, diag: MiniLoRaDiag, raw: ByteArray,
             receivedAtMs: Long, deviceKey: String = "") {
        try {
            val file = fileFor(context, deviceKey)
            val obj = JSONObject().apply {
                put("diagBase64",
                    Base64.encodeToString(raw, Base64.NO_WRAP))
                put("receivedAtMs", receivedAtMs)
            }
            file.writeText(obj.toString(2))
        } catch (e: Exception) {
            Log.e(TAG, "Failed to save diag", e)
        }
    }

    /** The persisted diag + its receive time, or null when never received. */
    fun load(context: Context, deviceKey: String = ""): Pair<MiniLoRaDiag, Long>? {
        try {
            val file = fileFor(context, deviceKey)
            if (!file.exists()) return null
            val obj = JSONObject(file.readText())
            val raw = Base64.decode(obj.getString("diagBase64"), Base64.NO_WRAP)
            val diag = MiniLoRaDiag.fromBytes(raw) ?: return null
            return diag to obj.optLong("receivedAtMs", 0L)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load diag", e)
            return null
        }
    }
}
