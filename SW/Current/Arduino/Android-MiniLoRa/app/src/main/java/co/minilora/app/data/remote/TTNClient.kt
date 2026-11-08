package co.minilora.app.data.remote

import android.util.Base64
import android.util.Log
import co.minilora.app.data.model.MiniLoRaDiag
import co.minilora.app.data.model.MiniLoRaPayload
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * HTTP client that fetches the latest uplink message from TTN v3 Application
 * Server storage API.
 *
 * Endpoint:
 *   GET /api/v3/as/applications/{app_id}/devices/{device_id}/packages/storage/uplink_message?limit=20&last=2h
 *
 * Response format: newline-delimited JSON (NDJSON) — each line is a separate {"result": {...}} object.
 * We parse every line and pick the one with the most recent received_at timestamp.
 */
object TTNClient {

    private const val TAG = "TTNClient"
    private const val TIMEOUT_SECONDS = 10L

    // Base URL — TTN Sandbox (free)
    //   https://{region}.cloud.thethings.network
    private var baseUrl: String = "https://eu1.cloud.thethings.network"

    private val client = OkHttpClient.Builder()
        .connectTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
        .readTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
        .build()

    fun setBaseUrl(url: String) {
        baseUrl = url.trimEnd('/')
    }

    /**
     * Fetches the latest uplink message from TTN for a given device.
     *
     * Response is NDJSON — each line is a complete JSON object.
     * We parse all lines and keep the most recent **fPort-1** frame with a
     * valid 28-byte payload. Diagnostics (fPort 2) and other frames are
     * skipped here — use [fetchLatestDiag] for those.
     */
    suspend fun fetchLatestUplink(        apiKey: String,
        appId: String,
        deviceId: String
    ): MiniLoRaPayload? {
        return fetchFrames(apiKey, appId, deviceId)
            ?.firstOrNull { it.first == 1 }
            ?.let { decodeTTNPayload(it.second) }
    }

    /**
     * Latest diagnostics frame (fPort 2, schema 0x03), or null.
     */
    suspend fun fetchLatestDiag(
        apiKey: String,
        appId: String,
        deviceId: String
    ): MiniLoRaDiag? {
        return fetchFrames(apiKey, appId, deviceId)
            ?.firstOrNull { it.first == 2 }
            ?.let { decodeTTNDiag(it.second) }
    }

    /**
     * History backfill: the last [limit] fPort-1 uplinks with their TTN
     * receive times (epoch s). Lets the tracker page show frames received
     * while the app was closed — the storage API holds ~24 h (free tier),
     * the frames carry raw GPS time since 2.6.8, and no-fix frames fall
     * back to the TTN receive time.
     */
    suspend fun fetchRecentUplinks(
        apiKey: String,
        appId: String,
        deviceId: String,
        limit: Int = 10,
    ): List<Pair<MiniLoRaPayload, Long>> {
        return fetchFrames(apiKey, appId, deviceId)
            ?.filter { it.first == 1 }
            ?.take(limit)
            ?.mapNotNull { (_, b64, ts) ->
                decodeTTNPayload(b64)?.let { it to ts }
            }
            ?: emptyList()
    }

    /**
     * One storage-API call, three uses: the history backfill (fPort-1
     * frames), the latest diag (fPort-2 frames) and the downlink-queue
     * state (the max received_at marks a pending downlink as delivered).
     * Returns (fPort, frm_payload base64, received_at epoch seconds),
     * newest first — empty when nothing is available.
     */
    suspend fun fetchRecentFrames(
        apiKey: String,
        appId: String,
        deviceId: String,
    ): List<Triple<Int, String, Long>> =
        fetchFrames(apiKey, appId, deviceId) ?: emptyList()

    /**
     * Latest diagnostics frame with its raw bytes (the VM persists the raw
     * frame so the diag survives app restarts — see DiagStore) plus the
     * frame's TTN receive time (≈ when the DEVICE transmitted it, epoch s;
     * 0 if the API omitted it).
     */
    suspend fun fetchLatestDiagRaw(
        apiKey: String,
        appId: String,
        deviceId: String
    ): Triple<MiniLoRaDiag, ByteArray, Long>? {
        val frame = fetchFrames(apiKey, appId, deviceId)
            ?.firstOrNull { it.first == 2 } ?: return null
        val bytes = base64ToBytes(frame.second) ?: return null
        return MiniLoRaDiag.fromBytes(bytes)
            ?.let { Triple(it, bytes, frame.third) }
    }

    /**
     * Push a downlink to the device's queue (TTN v3 `downlink/push`).
     * Used for the remote commands (fPort 3) — see ICD §3.
     *
     * @return true if TTN accepted the downlink (HTTP 200/201).
     */
    suspend fun sendDownlink(
        apiKey: String,
        appId: String,
        deviceId: String,
        fPort: Int,
        bytes: ByteArray
    ): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                val url = "$baseUrl/api/v3/as/applications/$appId/devices/$deviceId/down/push"
                val frmPayload = Base64.encodeToString(bytes, Base64.NO_WRAP)
                val json = JSONObject().put(
                    "downlinks",
                    org.json.JSONArray().put(
                        JSONObject()
                            .put("f_port", fPort)
                            .put("frm_payload", frmPayload)
                            // TTS expects the TxSchedulePriority enum name
                            // (UPPERCASE); "normal" is a 400.
                            .put("priority", "NORMAL")
                    )
                )
                val request = Request.Builder()
                    .url(url)
                    .addHeader("Authorization", "Bearer $apiKey")
                    .post(json.toString().toRequestBody("application/json".toMediaType()))
                    .build()
                val response = client.newCall(request).execute()
                val ok = response.isSuccessful
                val errBody = if (!ok) {
                    try { response.body?.string()?.take(400) } catch (_: Exception) { null }
                } else null
                response.body?.close()
                if (ok) {
                    Log.d(TAG, "downlink push fPort=$fPort ok")
                } else {
                    Log.w(TAG, "downlink push fPort=$fPort FAILED ${response.code}: $errBody")
                }
                ok
            } catch (e: Exception) {
                Log.e(TAG, "downlink push failed: ${e.message}", e)
                false
            }
        }
    }

    /**
     * Clear the device's entire TTN downlink queue (`downlink/replace` with
     * an empty queue) — cancels any pending command, wherever it was queued
     * from (app, server, console).
     *
     * @return true if TTN accepted the clear (HTTP 200/201).
     */
    suspend fun clearDownlinkQueue(
        apiKey: String,
        appId: String,
        deviceId: String
    ): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                val url = "$baseUrl/api/v3/as/applications/$appId/devices/$deviceId/down/replace"
                val json = JSONObject().put("downlinks", org.json.JSONArray())
                val request = Request.Builder()
                    .url(url)
                    .addHeader("Authorization", "Bearer $apiKey")
                    .post(json.toString().toRequestBody("application/json".toMediaType()))
                    .build()
                val response = client.newCall(request).execute()
                val ok = response.isSuccessful
                val errBody = if (!ok) {
                    try { response.body?.string()?.take(400) } catch (_: Exception) { null }
                } else null
                response.body?.close()
                if (ok) {
                    Log.d(TAG, "downlink queue cleared")
                } else {
                    Log.w(TAG, "downlink clear FAILED ${response.code}: $errBody")
                }
                ok
            } catch (e: Exception) {
                Log.e(TAG, "downlink clear failed: ${e.message}", e)
                false
            }
        }
    }

    /** (fPort, base64 frm_payload) pairs, newest first. */
    private suspend fun fetchFrames(
        apiKey: String,
        appId: String,
        deviceId: String
    ): List<Triple<Int, String, Long>>? = withContext(Dispatchers.IO) {
        try {
            val url = "$baseUrl/api/v3/as/applications/$appId/devices/$deviceId/packages/storage/uplink_message"
            // order=-received_at is MANDATORY: TTN returns oldest-first by
            // default, so a plain limit=20 keeps returning the same oldest
            // frames once the window holds more than 20 uplinks (the same
            // bug that stalled the server-side poller).
            // last=24h + limit=50: deep enough to BACKFILL an app-closed
            // period (50 × the 15-min cycle ≈ 12 h) instead of punching a
            // hole into the graphs — the free tier holds 24 h, and the
            // merge is idempotent (≤10 s dedup), so over-pulling is free.
            Log.d(TAG, "Fetching TTN uplink: $url?limit=50&last=24h&order=-received_at")

            val request = Request.Builder()
                .url("$url?limit=50&last=24h&order=-received_at")
                .addHeader("Authorization", "Bearer $apiKey")
                .get()
                .build()

            val response = client.newCall(request).execute()
            val body = response.body?.string() ?: return@withContext null

            if (!response.isSuccessful) {
                Log.w(TAG, "TTN API error ${response.code}: ${body.take(200)}")
                return@withContext null
            }

            Log.d(TAG, "Raw response length=${body.length} lines=${body.count { it == '\n' } + 1}")

            // Split into lines — each line is a separate JSON object (NDJSON)
            val lines = body.split("\n")
            val frames = mutableListOf<Triple<Int, String, Long>>()
            for (line in lines) {
                val trimmed = line.trim()
                if (trimmed.isEmpty()) continue
                val obj = try {
                    JSONObject(trimmed)
                } catch (_: Exception) {
                    continue
                }
                val msg = extractUplinkMessage(obj) ?: continue
                val frmPayload = msg.optString("frm_payload", "")
                if (frmPayload.isEmpty()) continue
                frames.add(Triple(msg.optInt("f_port", 1), frmPayload,
                                  extractReceivedAt(msg) / 1000))
            }
            frames
        } catch (e: Exception) {
            Log.e(TAG, "TTN fetch failed: ${e.message}", e)
            null
        }
    }

    /**
     * Extracts the received_at timestamp from an uplink_message JSONObject (ISO 8601).
     * Returns 0 if missing.
     */
    private fun extractReceivedAt(msg: JSONObject): Long {
        val timeStr = msg.optString("received_at", "") ?: return 0L
        if (timeStr.isEmpty()) return 0L
        // Strip fractional seconds beyond 3 digits — SimpleDateFormat can only parse milliseconds
        val cleaned = timeStr.replace(Regex("\\.(\\d{3})\\d+Z"), ".$1Z")
        return try {
            java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", java.util.Locale.US).parse(cleaned)?.time ?: 0L
        } catch (_: Exception) {
            try {
                java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", java.util.Locale.US).parse(cleaned)?.time ?: 0L
            } catch (_: Exception) {
                0L
            }
        }
    }

    /**
     * Extracts the uplink_message JSONObject from a TTN response element.
     * Handles {"result": {"uplink_message": {...}}} format.
     */
    private fun extractUplinkMessage(obj: JSONObject): JSONObject? {
        return if (obj.has("result")) {
            obj.getJSONObject("result").optJSONObject("uplink_message")
        } else {
            obj.optJSONObject("uplink_message")
        }
    }
}
