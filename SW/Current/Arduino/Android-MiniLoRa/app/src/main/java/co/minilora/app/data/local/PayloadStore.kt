package co.minilora.app.data.local

import android.content.Context
import android.util.Base64
import android.util.Log
import co.minilora.app.data.model.MiniLoRaPayload
import co.minilora.app.ui.model.DataSourceFlags
import co.minilora.app.ui.model.PayloadHistoryEntry
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Persists the last 400 payload entries as a JSON file in internal storage.
 * Each entry stores the raw 28-byte payload (base64) + timeUnix + source.
 * 400 × the tracker's 15-min cycle ≈ 4 days — deep enough that reopening
 * the app does not punch a hole into the graphs (the pre-400 cap of 10
 * entries only held ~2.5 h, so every app-closed period became a gap).
 */
object PayloadStore {

    private const val TAG = "PayloadStore"
    private const val MAX_ENTRIES = 400
    private const val FILE_NAME = "payload_history.json"

    private data class EntryData(
        val payloadBase64: String,
        val timeUnix: Long,
        val sourceName: String
    )

    fun save(context: Context, entries: List<PayloadHistoryEntry>) {
        try {
            val file = File(context.filesDir, FILE_NAME)
            val arr = JSONArray()
            for (e in entries.take(MAX_ENTRIES)) {
                val obj = JSONObject().apply {
                    put("payloadBase64", Base64.encodeToString(e.payload.rawBytesData, Base64.NO_WRAP))
                    put("timeUnix", e.timeUnix)
                    put("source", e.source.name)
                    put("deviceKey", e.deviceKey)
                }
                arr.put(obj)
            }
            file.writeText(arr.toString(2))
            Log.d(TAG, "Saved ${entries.size} entries to ${file.absolutePath}")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to save payload history", e)
        }
    }

    fun load(context: Context): List<PayloadHistoryEntry> {
        try {
            val file = File(context.filesDir, FILE_NAME)
            if (!file.exists()) return emptyList()

            val text = file.readText()
            val arr = JSONArray(text)
            val result = mutableListOf<PayloadHistoryEntry>()

            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                val entryData = EntryData(
                    payloadBase64 = obj.getString("payloadBase64"),
                    timeUnix = obj.getLong("timeUnix"),
                    sourceName = obj.getString("source")
                )
                val rawBytes = Base64.decode(entryData.payloadBase64, Base64.NO_WRAP)
                if (rawBytes.size != MiniLoRaPayload.PAYLOAD_SIZE) continue
                val payload = MiniLoRaPayload(rawBytes)
                val source = try {
                    DataSourceFlags.valueOf(entryData.sourceName)
                } catch (_: Exception) {
                    DataSourceFlags.BLE
                }
                result.add(PayloadHistoryEntry(payload, entryData.timeUnix, source))
            }

            Log.d(TAG, "Loaded ${result.size} entries from ${file.absolutePath}")
            return result
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load payload history", e)
            return emptyList()
        }
    }
}
