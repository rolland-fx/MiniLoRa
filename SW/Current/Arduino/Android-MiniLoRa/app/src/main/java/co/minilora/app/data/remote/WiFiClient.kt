package co.minilora.app.data.remote

import android.util.Log
import co.minilora.app.data.model.MiniLoRaDiag
import co.minilora.app.data.model.MiniLoRaPayload
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * HTTP client that fetches the latest stored reading from the MiniLoRa-server.
 *
 * Endpoint (MiniLoRa-server, NOT the old v1 test server's /last):
 *   GET http://{server_ip}:{port}/api/latest/{device}
 *
 * The response is the newest readings row as JSON; the payload is
 * reconstructed from its `raw_hex` field with the same parser used for
 * BLE beacons, so no lossy field-by-field re-encoding happens here.
 */
object WiFiClient {

    private const val TAG = "WiFiClient"
    private const val TIMEOUT_SECONDS = 10L
    private const val DEFAULT_PORT = 5000

    private val client = OkHttpClient.Builder()
        .connectTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
        .readTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
        .build()

    /** Bounded body read (W4): a captive portal or misbehaving proxy can
     *  return megabytes of HTML on these LAN endpoints — never buffer
     *  more than 2 MB. */
    private fun okhttp3.Response.bodyBounded(maxChars: Long = 2_000_000L): String? {
        val raw = body ?: return null
        if (raw.contentLength() > maxChars) {
            Log.w(TAG, "response too large (${raw.contentLength()} B) — rejected")
            return null
        }
        val text = raw.string()
        if (text.length > maxChars) {
            Log.w(TAG, "response too large (${text.length} chars) — rejected")
            return null
        }
        return text
    }

    /**
     * Fetches the latest stored reading from the MiniLoRa-server.
     *
     * @param serverIp the server's IP address (e.g. "192.168.1.100")
     * @param serverPort the server's HTTP port (default 5000)
     * @param deviceId the device name known to the server (e.g. "MiniLoRa-FX-1")
     * @return a parsed MiniLoRaPayload, or null if fetch/parse failed
     */
    suspend fun fetchLatestPayload(
        serverIp: String,
        serverPort: Int = DEFAULT_PORT,
        deviceId: String,
    ): MiniLoRaPayload? = withContext(Dispatchers.IO) {
        try {
            val url = "http://$serverIp:$serverPort/api/latest/" +
                    java.net.URLEncoder.encode(deviceId, "UTF-8")
                    .replace("+", "%20")
            Log.d(TAG, "Fetching WiFi payload: $url")

            val request = Request.Builder()
                .url(url)
                .get()
                .build()

            val response = client.newCall(request).execute()
            val body = response.bodyBounded() ?: return@withContext null

            if (response.code == 404) {
                Log.d(TAG, "Server has no data for $deviceId yet")
                return@withContext null
            }
            if (!response.isSuccessful) {
                Log.w(TAG, "WiFi API error ${response.code}: ${body.take(200)}")
                return@withContext null
            }

            val obj = try {
                JSONObject(body)
            } catch (_: Exception) {
                Log.w(TAG, "Failed to parse response as JSON")
                return@withContext null
            }

            // The row carries the raw 28-byte uplink as hex — decode it with
            // the exact same parser the BLE beacon path uses. Field-by-field
            // reconstruction (the old approach) was lossy and wrong.
            val rawHex = obj.optString("raw_hex", "")
            if (rawHex.length < 56) {
                Log.w(TAG, "No usable raw_hex in response (len ${rawHex.length})")
                return@withContext null
            }
            val buf = ByteArray(rawHex.length / 2)
            for (i in buf.indices) {
                buf[i] = rawHex.substring(i * 2, i * 2 + 2).toInt(16).toByte()
            }
            MiniLoRaPayload(buf)
        } catch (e: Exception) {
            Log.e(TAG, "WiFi fetch failed: ${e.message}", e)
            null
        }
    }

    private fun hexToBytes(hex: String): ByteArray {
        val buf = ByteArray(hex.length / 2)
        for (i in buf.indices) {
            buf[i] = hex.substring(i * 2, i * 2 + 2).toInt(16).toByte()
        }
        return buf
    }

    /**
     * History backfill: the last [limit] readings from the server (raw_hex
     * → the same byte-exact parser), covering app-off periods. The server
     * stores everything its TTN poller and the WiFi pushes delivered, so
     * this reconstructs days of timeline — the phone merges it idempotently.
     *
     * @return pairs of payload + the server's seen_at (epoch s)
     */
    suspend fun fetchRecentPayloads(
        serverIp: String,
        serverPort: Int = DEFAULT_PORT,
        deviceId: String,
        limit: Int = 200,
    ): List<Pair<MiniLoRaPayload, Long>> = withContext(Dispatchers.IO) {
        try {
            val url = "http://$serverIp:$serverPort/api/recent/" +
                    java.net.URLEncoder.encode(deviceId, "UTF-8")
                        .replace("+", "%20") + "?limit=$limit"
            val request = Request.Builder().url(url).get().build()
            val response = client.newCall(request).execute()
            val body = response.bodyBounded() ?: return@withContext emptyList()
            if (!response.isSuccessful) {
                Log.w(TAG, "WiFi recent API error ${response.code}: ${body.take(200)}")
                return@withContext emptyList()
            }
            val arr = try {
                org.json.JSONArray(body)
            } catch (_: Exception) {
                Log.w(TAG, "Recent response is not a JSON array")
                return@withContext emptyList()
            }
            val out = mutableListOf<Pair<MiniLoRaPayload, Long>>()
            for (i in 0 until arr.length()) {
                val obj = arr.optJSONObject(i) ?: continue
                val rawHex = obj.optString("raw_hex", "")
                if (rawHex.length < 56) continue
                val seenAt = obj.optLong("seen_at", 0L)
                out.add(MiniLoRaPayload(hexToBytes(rawHex)) to seenAt)
            }
            out
        } catch (e: Exception) {
            Log.e(TAG, "WiFi recent fetch failed: ${e.message}", e)
            emptyList()
        }
    }

    /** Device list from the MiniLoRa-server (/api/devices) — used for the
     *  identity→group mapping ([device_groups]) and the known-devices set. */
    data class ServerDevice(val deviceId: String, val group: String?)

    suspend fun fetchDevices(
        serverIp: String,
        serverPort: Int = DEFAULT_PORT,
    ): List<ServerDevice> = withContext(Dispatchers.IO) {
        try {
            val url = "http://$serverIp:$serverPort/api/devices"
            val request = Request.Builder().url(url).get().build()
            val response = client.newCall(request).execute()
            val body = response.bodyBounded() ?: return@withContext emptyList()
            if (!response.isSuccessful) return@withContext emptyList()
            val arr = org.json.JSONArray(body)
            val out = mutableListOf<ServerDevice>()
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val id = o.optString("device_id", "")
                if (id.isBlank()) continue
                out.add(ServerDevice(id, o.optString("group", "").ifBlank { null }))
            }
            out
        } catch (e: Exception) {
            Log.e(TAG, "device list fetch failed: ${e.message}", e)
            emptyList()
        }
    }

    /**
     * Latest diagnostics frame from the server (rows are newest-first),
     * with its raw bytes for DiagStore persistence and the row's seen_at
     * (≈ when the DEVICE sent it, epoch s). Null when none stored.
     */
    suspend fun fetchLatestDiag(
        serverIp: String,
        serverPort: Int = DEFAULT_PORT,
        deviceId: String,
    ): Triple<MiniLoRaDiag, ByteArray, Long>? = withContext(Dispatchers.IO) {
        try {
            val url = "http://$serverIp:$serverPort/api/diagnostics/" +
                    java.net.URLEncoder.encode(deviceId, "UTF-8")
                        .replace("+", "%20")
            val request = Request.Builder().url(url).get().build()
            val response = client.newCall(request).execute()
            val body = response.bodyBounded() ?: return@withContext null
            if (!response.isSuccessful) return@withContext null
            val arr = try {
                org.json.JSONArray(body)
            } catch (_: Exception) {
                return@withContext null
            }
            if (arr.length() == 0) return@withContext null
            val row = arr.getJSONObject(0)
            val rawHex = row.optString("raw_hex", "")
            if (rawHex.length < 58) return@withContext null  // 29 B min (0x05)
            val buf = hexToBytes(rawHex)
            val seenAt = row.optLong("seen_at", 0L)
            MiniLoRaDiag.fromBytes(buf)?.let { Triple(it, buf, seenAt) }
        } catch (e: Exception) {
            Log.e(TAG, "WiFi diag fetch failed: ${e.message}", e)
            null
        }
    }
}
