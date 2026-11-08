package co.minilora.app.ui

import android.app.Application
import android.bluetooth.BluetoothManager
import android.content.Context
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import co.minilora.app.ble.BLEDeviceConfigurator
import co.minilora.app.ble.BleScanner
import co.minilora.app.data.local.DiagStore
import co.minilora.app.data.local.DownlinkEntry
import co.minilora.app.data.local.DownlinkStore
import co.minilora.app.data.local.PayloadStore
import co.minilora.app.data.model.MiniLoRaCommands
import co.minilora.app.data.model.MiniLoRaDiag
import co.minilora.app.data.model.MiniLoRaPayload
import co.minilora.app.data.remote.TTNClient
import co.minilora.app.data.remote.WiFiClient
import co.minilora.app.data.remote.base64ToBytes
import co.minilora.app.data.remote.decodeTTNPayload
import co.minilora.app.ui.model.DataSourceFlags
import co.minilora.app.ui.model.PayloadHistoryEntry
import co.minilora.app.ui.model.PruneOption
import co.minilora.app.util.DeviceConfig
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

/**
 * Single state holder for the app. All data layers (BLE scan/config, TTN,
 * WiFi server, history store) hang off this; the Compose UI only renders.
 */
class AppViewModel(app: Application) : AndroidViewModel(app) {

    // ---- BLE scan (beacon reception) ----
    private val btManager =
        app.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    val bleAdapter = btManager?.adapter
    private val scanner = BleScanner(bleAdapter)

    val scanHits = scanner.hits
    val scanActive = scanner.scanning
    val lastBeacon = scanner.lastPayload

    // ---- history ----
    private val _history = MutableStateFlow<List<PayloadHistoryEntry>>(emptyList())
    val history: StateFlow<List<PayloadHistoryEntry>> = _history

    private val _snack = MutableStateFlow<String?>(null)
    val snack: StateFlow<String?> = _snack
    fun consumeSnack() { _snack.value = null }
    private fun toast(msg: String) { _snack.value = msg }

    // ---- BLE config/status ops ----
    private val configurator = BLEDeviceConfigurator(app)
    private val _statusFrame = MutableStateFlow<Map<String, Any>?>(null)
    val statusFrame: StateFlow<Map<String, Any>?> = _statusFrame
    private val _bleBusy = MutableStateFlow(false)
    val bleBusy: StateFlow<Boolean> = _bleBusy

    // ---- TTN / WiFi ----
    private val _diag = MutableStateFlow<MiniLoRaDiag?>(null)
    val diag: StateFlow<MiniLoRaDiag?> = _diag
    // When the displayed diag frame was received (epoch ms) — shown on the
    // Remote tab and persisted across restarts with the frame itself.
    private val _diagReceivedAt = MutableStateFlow<Long?>(null)
    val diagReceivedAt: StateFlow<Long?> = _diagReceivedAt
    // The app's downlink outbox: what was queued via TTN and its state
    // (pending → sent on the next uplink; superseded sends are "replaced").
    private val _downlinks = MutableStateFlow<List<DownlinkEntry>>(emptyList())
    val downlinks: StateFlow<List<DownlinkEntry>> = _downlinks
    private val _ttnPolling = MutableStateFlow(false)
    val ttnPolling: StateFlow<Boolean> = _ttnPolling
    private val _wifiPolling = MutableStateFlow(false)
    val wifiPolling: StateFlow<Boolean> = _wifiPolling
    private val _blePolling = MutableStateFlow(false)
    val blePolling: StateFlow<Boolean> = _blePolling
    private var ttnJob: Job? = null
    private var wifiJob: Job? = null
    private var bleJob: Job? = null

    init {
        viewModelScope.launch {
            val saved = PayloadStore.load(app)
            if (saved.isNotEmpty()) _history.value = saved
        }
        // The last diag survives restarts: shown until a new one arrives.
        DiagStore.load(app)?.let { (diag, at) ->
            _diag.value = diag
            _diagReceivedAt.value = at
        }
        _downlinks.value = DownlinkStore.load(app)
        purgeExpiredDownlinks()  // yesterday's receipts start empty
        // Beacon reception doubles as telemetry: every parsed advertisement
        // lands in the history (same behaviour as the v1 app).
        viewModelScope.launch {
            scanner.lastPayload.collect { hit ->
                hit?.payload?.let { addHistory(it, DataSourceFlags.BLE) }
            }
        }
        // One-shot fetches at startup
        if (DeviceConfig.ttnConfigured()) {
            viewModelScope.launch { fetchTTNNow() }
        }
        if (DeviceConfig.wifiConfigured()) {
            viewModelScope.launch { fetchWiFiNow() }
        }
        // Polling auto-resume: all three toggles are persisted (they live in
        // Settings — app behavior, not device control), so a restarted app
        // goes straight back to its configured feeds.
        if (DeviceConfig.ttnPollingEnabled && DeviceConfig.ttnConfigured()) {
            _ttnPolling.value = true
            ttnJob = viewModelScope.launch {
                while (true) {
                    fetchTTNNow()
                    delay(DeviceConfig.ttnPollIntervalSeconds * 1000L)
                }
            }
        }
        if (DeviceConfig.wifiPollingEnabled && DeviceConfig.wifiConfigured()) {
            _wifiPolling.value = true
            wifiJob = viewModelScope.launch {
                while (true) {
                    fetchWiFiNow()
                    delay(DeviceConfig.wifiPollIntervalSeconds * 1000L)
                }
            }
        }
        if (DeviceConfig.blePollingEnabled) {
            _blePolling.value = true
            bleJob = viewModelScope.launch { blePollLoop() }
        }
    }

    // ===== scanning =====

    fun toggleScan() {
        if (scanActive.value) {
            scanner.stop()
        } else {
            scanner.start(
                DeviceConfig.targetDeviceName, DeviceConfig.targetMacAddress
            ) { toast(it) }
        }
    }

    fun selectDevice(name: String, address: String) {
        DeviceConfig.targetDeviceName = name
        DeviceConfig.targetMacAddress = address
        DeviceConfig.save()
        toast("Selected: $name")
    }

    fun stopScanOnPause() = scanner.stop()

    // ===== history =====

    private fun addHistory(payload: MiniLoRaPayload, source: DataSourceFlags) {
        // GPS time when the payload carries it; otherwise the receive
        // wall-clock — a no-fix payload must still sort and merge by when
        // it was heard, not by 1970 (time 0).
        // Server-defense twin (db.py nulls future gps_time): a stamp more
        // than 10 min in the future is a net-time fallback overshoot
        // (pre-2.6.7) — display the receive time instead of a lie.
        val nowS = System.currentTimeMillis() / 1000
        val ts = if (payload.timeUnixSeconds in 1..(nowS + 600))
                     payload.timeUnixSeconds
                 else nowS
        _history.value = buildList {
            // Feeds reporting the same device wake within 10 s of each
            // other (BLE beacon + TTN downlink + server push of the same
            // cycle) collapse into ONE card showing the combined sources.
            val existing = _history.value.firstOrNull {
                Math.abs(it.timeUnix - ts) <= 10
            }
            if (existing != null) {
                val combined = existing.source.combine(source)
                val merged = existing.copy(
                    source = combined,
                    payload = payload,
                    timeUnix = if (payload.timeUnixSeconds > 0)
                        payload.timeUnixSeconds else existing.timeUnix,
                )
                add(merged)
                addAll(_history.value.filter { it !== existing })
            } else {
                add(PayloadHistoryEntry(payload, ts, source))
                addAll(_history.value)
            }
        }
        // debounced persist
        if (saveJob?.isActive != true) {
            saveJob = viewModelScope.launch {
                delay(5000)
                PayloadStore.save(getApplication(), _history.value)
            }
        }
    }

    /**
     * Backfill from TTN/WiFi pulls: frames received while the app was
     * closed. Unlike addHistory (live feeds — newest at front), this is
     * position-preserving: an entry already in the list is left where it
     * is (only its source set grows), and a new one is inserted at its
     * chronological spot. Re-pulls are therefore idempotent. Each entry's
     * stamp is the payload's GPS time when plausible, else the pull
     * source's own receive/seen time (TTN received_at, server seen_at).
     */
    private fun mergeHistory(
        entries: List<Triple<MiniLoRaPayload, Long, DataSourceFlags>>,
    ) {
        if (entries.isEmpty()) return
        var list = _history.value
        val nowS = System.currentTimeMillis() / 1000
        for ((payload, fallbackTs, source) in entries) {
            val ts = when {
                payload.timeUnixSeconds in 1..(nowS + 600) ->
                    payload.timeUnixSeconds
                fallbackTs > 0 -> fallbackTs
                else -> nowS
            }
            val existing = list.firstOrNull { Math.abs(it.timeUnix - ts) <= 10 }
            list = if (existing != null) {
                list.map {
                    if (it === existing) it.copy(source = it.source.combine(source))
                    else it
                }
            } else {
                (list + PayloadHistoryEntry(payload, ts, source))
                    .sortedByDescending { it.timeUnix }
            }
        }
        _history.value = list
        // debounced persist — same as addHistory
        if (saveJob?.isActive != true) {
            saveJob = viewModelScope.launch {
                delay(5000)
                PayloadStore.save(getApplication(), _history.value)
            }
        }
    }

    /**
     * Record a diagnostics frame: shown on the Remote tab until the next
     * one arrives, persisted (raw bytes + receive time) across restarts.
     *
     * The device reports under two identities (WiFi push name + TTN id).
     * After a remote-config change one identity can serve a STALE frame —
     * e.g. WiFi OFF silences the server's WiFi-path rows while TTN keeps
     * flowing — and blindly overwriting would flip the config toggles back
     * to the old state. bootCount is the device-side version: never regress.
     */
    private fun setDiag(diag: MiniLoRaDiag, raw: ByteArray, receivedAtMs: Long) {
        val cur = _diag.value
        if (cur != null && diag.bootCount < cur.bootCount) return
        _diag.value = diag
        _diagReceivedAt.value = receivedAtMs
        DiagStore.save(getApplication(), diag, raw, receivedAtMs)
    }

    /** Record a successfully queued downlink; a previous pending one is
     *  superseded (TTN's downlink/push replaces the class-A queue). */
    private fun recordDownlink(label: String, bytes: ByteArray) {
        val hex = bytes.joinToString(" ") { "%02X".format(it) }
        val now = System.currentTimeMillis()
        _downlinks.value = buildList {
            addAll(_downlinks.value.map {
                if (it.status == "pending") it.copy(status = "replaced") else it
            })
            add(DownlinkEntry(label, hex, now, "pending"))
        }.takeLast(12)
        DownlinkStore.save(getApplication(), _downlinks.value)
    }

    /** Called after each pull: an uplink received at/after the queue time
     *  carried the pending frame in its RX window — mark it sent, stamped
     *  with THAT uplink's time (the honest delivery moment, not the queue
     *  time). */
    private fun markDownlinksSent(newestUplinkSec: Long) {
        if (newestUplinkSec <= 0) return
        var changed = false
        _downlinks.value = _downlinks.value.map {
            if (it.status == "pending" && newestUplinkSec * 1000 >= it.queuedAtMs) {
                changed = true
                it.copy(status = "sent", sentAtMs = newestUplinkSec * 1000)
            } else it
        }
        if (changed) DownlinkStore.save(getApplication(), _downlinks.value)
        purgeExpiredDownlinks()
    }

    /** Terminal entries (sent/replaced) are receipts: kept 30 min so the
     *  outcome is visible, then dropped. Pending entries never expire. */
    fun purgeExpiredDownlinks() {
        val cutoff = System.currentTimeMillis() - 30 * 60_000L
        val filtered = _downlinks.value.filter {
            it.status == "pending" || it.queuedAtMs >= cutoff
        }
        if (filtered.size != _downlinks.value.size) {
            _downlinks.value = filtered
            DownlinkStore.save(getApplication(), _downlinks.value)
        }
    }

    /** Clear the device's TTN downlink queue: pending commands are cancelled
     *  wherever they were queued from (app, server, console) and marked
     *  "cleared" (they stay 30 min as receipts, like sent ones). */
    fun clearDownlinkQueue() {
        if (!DeviceConfig.ttnConfigured()) {
            toast("Configure TTN in settings first"); return
        }
        viewModelScope.launch {
            val region = DeviceConfig.ttnRegion.ifBlank { "eu1" }
            TTNClient.setBaseUrl("https://$region.cloud.thethings.network")
            val ok = TTNClient.clearDownlinkQueue(
                DeviceConfig.ttnApiKey, DeviceConfig.ttnAppId,
                DeviceConfig.ttnDeviceId.ifBlank { DeviceConfig.targetDeviceName },
            )
            if (ok) {
                var had = false
                _downlinks.value = _downlinks.value.map {
                    if (it.status == "pending") { had = true; it.copy(status = "cleared") }
                    else it
                }
                DownlinkStore.save(getApplication(), _downlinks.value)
                toast(if (had) "TTN queue cleared — pending commands cancelled"
                      else "TTN queue cleared (nothing was pending)")
            } else {
                toast("Clear failed")
            }
        }
    }

    /**
     * Local twin of the server's /api/device/{id}/prune: drop history by
     * window (cutoff = now − age, matching the server), keep only the
     * newest entry, or wipe everything. Persisted immediately (the
     * debounced addHistory save would otherwise resurrect stale rows).
     * Returns the number of entries deleted.
     */
    fun pruneHistory(option: PruneOption): Int {
        val before = _history.value.size
        val now = System.currentTimeMillis() / 1000
        _history.value = when (option) {
            PruneOption.ALL -> emptyList()
            PruneOption.LAST ->
                listOfNotNull(_history.value.maxByOrNull { it.timeUnix })
            else -> {
                val ageS = when (option) {
                    PruneOption.HOUR -> 3600L
                    PruneOption.DAY -> 86_400L
                    PruneOption.WEEK -> 7 * 86_400L
                    else -> 30 * 86_400L   // MONTH
                }
                val cutoff = now - ageS
                _history.value.filter { it.timeUnix >= cutoff }
            }
        }
        val deleted = before - _history.value.size
        if (deleted > 0) {
            saveJob?.cancel()
            PayloadStore.save(getApplication(), _history.value)
        }
        return deleted
    }

    private var saveJob: Job? = null

    // ===== TTN =====

    fun toggleTTNPolling() {
        if (_ttnPolling.value) {
            ttnJob?.cancel(); ttnJob = null
            _ttnPolling.value = false
            DeviceConfig.ttnPollingEnabled = false
            DeviceConfig.save()
            toast("TTN polling stopped")
        } else {
            if (!DeviceConfig.ttnConfigured()) {
                toast("Configure TTN in settings first"); return
            }
            _ttnPolling.value = true
            DeviceConfig.ttnPollingEnabled = true
            DeviceConfig.save()
            ttnJob = viewModelScope.launch {
                while (true) {
                    fetchTTNNow()
                    delay(DeviceConfig.ttnPollIntervalSeconds * 1000L)
                }
            }
        }
    }

    suspend fun fetchTTNNow() {
        if (!DeviceConfig.ttnConfigured()) return
        val region = DeviceConfig.ttnRegion.ifBlank { "eu1" }
        TTNClient.setBaseUrl("https://$region.cloud.thethings.network")
        val dev = DeviceConfig.ttnDeviceId.ifBlank { DeviceConfig.targetDeviceName }
        // One storage-API call serves everything: the history backfill, the
        // latest diag, and the downlink-queue state (an uplink received at/
        // after the queue time carried the pending frame in its RX window).
        val frames = TTNClient.fetchRecentFrames(
            DeviceConfig.ttnApiKey, DeviceConfig.ttnAppId, dev,
        )
        frames.maxOfOrNull { it.third }?.let { markDownlinksSent(it) }
        frames.filter { it.first == 1 }.take(50).forEach { (_, b64, ts) ->
            decodeTTNPayload(b64)?.let {
                mergeHistory(listOf(Triple(it, ts, DataSourceFlags.TTN)))
            }
        }
        frames.firstOrNull { it.first == 2 }?.let { (_, b64, ts) ->
            base64ToBytes(b64)?.let { raw ->
                MiniLoRaDiag.fromBytes(raw)?.let { diag ->
                    // Stamp the DEVICE's send time (TTN received_at ≈ the
                    // radio transmission), not the phone's pull time.
                    val sentAtMs = if (ts > 0) ts * 1000
                                   else System.currentTimeMillis()
                    setDiag(diag, raw, sentAtMs)
                }
            }
        }
    }

    // ===== WiFi server =====

    fun toggleWiFiPolling() {
        if (_wifiPolling.value) {
            wifiJob?.cancel(); wifiJob = null
            _wifiPolling.value = false
            DeviceConfig.wifiPollingEnabled = false
            DeviceConfig.save()
            toast("WiFi polling stopped")
        } else {
            if (!DeviceConfig.wifiConfigured()) {
                toast("Configure WiFi server IP in settings first"); return
            }
            _wifiPolling.value = true
            DeviceConfig.wifiPollingEnabled = true
            DeviceConfig.save()
            wifiJob = viewModelScope.launch {
                while (true) {
                    fetchWiFiNow()
                    delay(DeviceConfig.wifiPollIntervalSeconds * 1000L)
                }
            }
        }
    }

    suspend fun fetchWiFiNow() {
        if (!DeviceConfig.wifiConfigured()) return
        val dev = DeviceConfig.targetDeviceName.ifBlank { "MiniLoRa-FX-1" }
        // History backfill: the server's last ~10 readings (raw_hex → the
        // same byte-exact parser), covering app-off periods.
        val recent = WiFiClient.fetchRecentPayloads(
            DeviceConfig.wifiServerIp, DeviceConfig.wifiServerPort, dev,
        )
        recent.maxOfOrNull { it.second }?.let { markDownlinksSent(it) }
        recent.forEach { (payload, seenAt) ->
            mergeHistory(listOf(Triple(payload, seenAt, DataSourceFlags.WIFI)))
        }
        WiFiClient.fetchLatestDiag(
            DeviceConfig.wifiServerIp, DeviceConfig.wifiServerPort, dev,
        )?.let { (diag, raw, seenAt) ->
            val sentAtMs = if (seenAt > 0) seenAt * 1000
                           else System.currentTimeMillis()
            setDiag(diag, raw, sentAtMs)
        }
    }

    // ===== BLE beacon polling =====

    /**
     * BLE polling (ICD §6): the tracker only advertises during its brief
     * wake window (~1-2 min every sleep cycle), so a continuous scan burns
     * phone battery listening to empty air. Duty-cycle the scanner instead:
     * burst-listen for BLE_BURST_MS, then idle for the rest of the interval.
     * Hits land in the history through the same collector as manual scans.
     * The on/off switch is persisted (DeviceConfig.blePollingEnabled) and
     * auto-resumed from init(), so a restarted app keeps listening. A pause
     * (stopScanOnPause) only kills the current burst — the next loop
     * iteration re-arms the scanner.
     */
    fun toggleBLEPolling() {
        if (_blePolling.value) {
            bleJob?.cancel(); bleJob = null
            scanner.stop()
            _blePolling.value = false
            DeviceConfig.blePollingEnabled = false
            DeviceConfig.save()
            toast("BLE polling stopped")
        } else {
            _blePolling.value = true
            DeviceConfig.blePollingEnabled = true
            DeviceConfig.save()
            bleJob = viewModelScope.launch { blePollLoop() }
            toast("BLE polling: waiting for " +
                DeviceConfig.targetDeviceName.ifBlank { "any MiniLoRa beacon" })
        }
    }

    private suspend fun blePollLoop() {
        val burstMs = 30_000L
        while (true) {
            if (!scanActive.value) {
                scanner.start(DeviceConfig.targetDeviceName,
                              DeviceConfig.targetMacAddress) { /* transient */ }
            }
            delay(burstMs)
            scanner.stop()
            val rest = DeviceConfig.blePollIntervalSeconds * 1000L - burstMs
            if (rest > 0) delay(rest)
        }
    }

    // ===== remote commands (ICD §3) =====

    fun sendCommand(opcode: Int, label: String, arg: Int = 0) {
        if (!DeviceConfig.ttnConfigured()) {
            toast("Configure TTN in settings first"); return
        }
        viewModelScope.launch {
            val region = DeviceConfig.ttnRegion.ifBlank { "eu1" }
            TTNClient.setBaseUrl("https://$region.cloud.thethings.network")
            val bytes = MiniLoRaCommands.frame(opcode, arg)
            val ok = TTNClient.sendDownlink(
                DeviceConfig.ttnApiKey, DeviceConfig.ttnAppId,
                DeviceConfig.ttnDeviceId.ifBlank { DeviceConfig.targetDeviceName },
                fPort = 3, bytes = bytes,
            )
            if (ok) recordDownlink(label, bytes)
            toast(if (ok) "$label queued" else "$label failed")
        }
    }

    /** CFG_SET (ICD 3.5 §3): toggle a transport/sensor on the device. The
     *  device confirms on the next diag frame's configFlags byte. */
    fun sendConfigSet(flagId: Int, on: Boolean, label: String) {
        if (!DeviceConfig.ttnConfigured()) {
            toast("Configure TTN in settings first"); return
        }
        viewModelScope.launch {
            val region = DeviceConfig.ttnRegion.ifBlank { "eu1" }
            TTNClient.setBaseUrl("https://$region.cloud.thethings.network")
            val bytes = MiniLoRaCommands.configSet(flagId, on)
            val ok = TTNClient.sendDownlink(
                DeviceConfig.ttnApiKey, DeviceConfig.ttnAppId,
                DeviceConfig.ttnDeviceId.ifBlank { DeviceConfig.targetDeviceName },
                fPort = 3, bytes = bytes,
            )
            if (ok) recordDownlink(label, bytes)
            toast(if (ok) "$label queued — confirm on next diag"
                  else "$label failed")
        }
    }

    // ===== BLE config ops (suspend wrappers around the callback API) =====

    private fun remoteDevice(mac: String) =
        bleAdapter?.getRemoteDevice(mac)

    suspend fun readDeviceStatusAndConfig(): Pair<Map<String, Any>?, Map<String, Any>?>? {
        val mac = DeviceConfig.targetMacAddress
        if (mac.isBlank()) { toast("No device selected"); return null }
        if (remoteDevice(mac) == null) return null
        _bleBusy.value = true
        try {
            val blob = connectAndUse { onReady ->
                // GATT allows one read at a time: status first, then the
                // config blob (chained in the callback). Old firmware without
                // the status characteristic answers with an empty frame.
                configurator.onStatusRead = { data ->
                    _statusFrame.value =
                        if (data.isNotEmpty()) configurator.parseStatus(data) else null
                    configurator.readConfig()
                }
                configurator.onReadComplete = { onReady(it) }
                configurator.readStatus()
            }
            if (blob.isEmpty()) return null to null
            return _statusFrame.value to configurator.parsePayload(blob)
        } catch (e: Exception) {
            toast("Read failed: ${e.message}")
            return null
        } finally {
            _bleBusy.value = false
        }
    }

    /** Parse a config envelope/body from bytes (file import). Null if invalid. */
    fun parseConfigBytes(bytes: ByteArray): Map<String, Any>? =
        configurator.parsePayload(bytes)

    suspend fun writeConfig(envelope: ByteArray): Boolean {
        val mac = DeviceConfig.targetMacAddress
        if (mac.isBlank()) { toast("No device selected"); return false }
        if (remoteDevice(mac) == null) return false
        _bleBusy.value = true
        try {
            return connectAndUse { onReady ->
                configurator.onWriteComplete = { onReady(it) }
                configurator.writeConfig(envelope)
            }.also { ok -> toast(if (ok) "Write successful" else "Write failed") }
        } catch (e: Exception) {
            toast("Write failed: ${e.message}")
            return false
        } finally {
            _bleBusy.value = false
        }
    }

    /**
     * Connect, run the single GATT op armed inside `arm` (exactly one
     * `onReady` invocation), then disconnect. Callbacks must be armed
     * before the op starts; connection success triggers `arm`.
     */
    private suspend fun <T> connectAndUse(arm: ((T) -> Unit) -> Unit): T =
        suspendCancellableCoroutine { cont ->
            cont.invokeOnCancellation { configurator.disconnect() }
            var armed = false
            configurator.onConnectionState = { connected ->
                when {
                    !connected -> if (cont.isActive) {
                        cont.cancel(IllegalStateException("Connection failed"))
                    }
                    !armed -> {
                        armed = true
                        arm { value ->
                            configurator.disconnect()
                            if (cont.isActive) cont.resume(value)
                        }
                    }
                }
            }
            val device = remoteDevice(DeviceConfig.targetMacAddress)
            if (device == null) {
                cont.cancel(IllegalStateException("No device"))
            } else {
                configurator.connect(device)
            }
        }
}
