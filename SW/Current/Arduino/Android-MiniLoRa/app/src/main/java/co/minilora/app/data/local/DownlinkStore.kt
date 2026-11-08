package co.minilora.app.data.local

import android.content.Context
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** One fPort-3 command the app queued via TTN, and its delivery state. */
data class DownlinkEntry(
    val label: String,      // human label, e.g. "Sleep 30 min"
    val hex: String,        // the frame, e.g. "4D 07 04"
    val queuedAtMs: Long,   // wall clock when TTN accepted it
    val status: String,     // pending | sent | replaced
    val sentAtMs: Long = 0, // wall clock of the uplink that confirmed
                            // delivery (the device pulled the frame in that
                            // uplink's RX window); 0 until sent
)

/**
 * Persists the app's downlink outbox (last ~12): what was queued, when,
 * and whether it was superseded by a newer command or delivered on an
 * uplink's RX window. TTN's class-A queue holds one pending frame per
 * device (downlink/push replaces), so at most one entry is "pending".
 */
object DownlinkStore {

    private const val TAG = "DownlinkStore"
    private const val FILE_NAME = "downlink_queue.json"
    private const val MAX_ENTRIES = 12

    fun save(context: Context, entries: List<DownlinkEntry>) {
        try {
            val file = File(context.filesDir, FILE_NAME)
            val arr = JSONArray()
            for (e in entries.take(MAX_ENTRIES)) {
                arr.put(JSONObject().apply {
                    put("label", e.label)
                    put("hex", e.hex)
                    put("queuedAtMs", e.queuedAtMs)
                    put("sentAtMs", e.sentAtMs)
                    put("status", e.status)
                })
            }
            file.writeText(arr.toString(2))
        } catch (e: Exception) {
            Log.e(TAG, "Failed to save downlink queue", e)
        }
    }

    fun load(context: Context): List<DownlinkEntry> {
        try {
            val file = File(context.filesDir, FILE_NAME)
            if (!file.exists()) return emptyList()
            val arr = JSONArray(file.readText())
            val out = mutableListOf<DownlinkEntry>()
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                out.add(DownlinkEntry(
                    label = o.optString("label", "?"),
                    hex = o.optString("hex", ""),
                    queuedAtMs = o.optLong("queuedAtMs", 0L),
                    sentAtMs = o.optLong("sentAtMs", 0L), // pre-2.6.16 rows: 0
                    status = o.optString("status", "pending"),
                ))
            }
            return out
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load downlink queue", e)
            return emptyList()
        }
    }
}
