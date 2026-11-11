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
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
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

    /** Trek peer contact state (ICD §10.9): the freshest relay per peer
     *  origin — the Map tab's contact card derives in-touch/lost from
     *  lastSeenMs freshness. Session-scoped: contact state is transient. */
    data class PeerContact(
        val originKey: String,        // %08X origin id
        val seq: Long,                // the peer's frame counter
        val payload: MiniLoRaPayload,
        val rssi: Int,
        val relayMac: String,         // the tracker that relayed it
        val lastSeenMs: Long,         // phone wall-clock
    )

    private val _peerContacts =
        MutableStateFlow<Map<String, PeerContact>>(emptyMap())
    val peerContacts: StateFlow<Map<String, PeerContact>> = _peerContacts

    // ---- multi-tracker: identity filter + per-device display ----
    data class DeviceChip(val label: String, val key: String)  // key "" = All

    private val _selectedDeviceKey = MutableStateFlow("")
    val selectedDeviceKey: StateFlow<String> = _selectedDeviceKey

    /** History filtered to the selected device/group ("" = all trackers).
     *  A group label matches every identity mapped to it. */
    val displayHistory: StateFlow<List<PayloadHistoryEntry>> =
        combine(_history, _selectedDeviceKey) { h, k ->
            if (k.isBlank()) h
            else h.filter {
                it.deviceKey == k || DeviceConfig.deviceGroupMap[it.deviceKey] == k
            }
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000),
                  emptyList())

    /** Filter chips: "All" + one per group label or bare identity. */
    val deviceChips: StateFlow<List<DeviceChip>> = _history.map { entries ->
        val labels = linkedMapOf<String, String>()   // label -> representative key
        entries.forEach { e ->
            val label = displayNameOf(e.deviceKey).takeIf { it.isNotBlank() }
                        ?: e.deviceKey
            if (label.isNotBlank()) labels.putIfAbsent(label, e.deviceKey)
        }
        buildList {
            add(DeviceChip("All", ""))
            labels.forEach { (label, key) -> add(DeviceChip(label, key)) }
        }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    fun selectDeviceFilter(key: String) {
        _selectedDeviceKey.value = key
        if (key.isBlank()) return
        // the selected tracker's stored diag is the one on display
        val stored = DiagStore.load(getApplication(), key)
        _diag.value = stored?.first
        _diagReceivedAt.value = stored?.second
        _diagDeviceKey.value = key
    }

    /** Display name for any identity: a group label from the server, a
     *  friendly "Peer XXXXXXXX" for trek peer relays (§10.9), else the raw
     *  key (BLE MAC / TTN id / push name). */
    fun displayNameOf(deviceKey: String): String {
        DeviceConfig.deviceGroupMap[deviceKey]?.takeIf { it.isNotBlank() }
            ?.let { return it }
        val hex = deviceKey.removePrefix("peer:")
        if (hex.length == 8 && hex.all { it.isDigit() || it in 'A'..'F' })
            return "Peer $hex"
        return deviceKey
    }

    private val _snack = MutableStateFlow<String?>(null)
    val snack: StateFlow<String?> = _snack
    fun consumeSnack() { _snack.value = null }
    private fun toast(msg: String) { _snack.value = msg }

    // Optional snackbar action (e.g. "Undo" on a device re-target): the
    // host shows it with the next snack and clears it after, so unrelated
    // toasts never inherit a stale button.
    var snackActionLabel: String? = null
        private set
    private var snackAction: (() -> Unit)? = null

    fun showUndoableSnack(msg: String, actionLabel: String,
                          action: () -> Unit) {
        snackActionLabel = actionLabel
        snackAction = action
        _snack.value = msg
    }

    fun performSnackAction() {
        snackAction?.invoke()
        snackAction = null
        snackActionLabel = null
    }

    fun clearSnackAction() {
        snackAction = null
        snackActionLabel = null
    }

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
    private val _diagDeviceKey = MutableStateFlow("")
    val diagDeviceKey: StateFlow<String> = _diagDeviceKey
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
            // v1 entries carried no identity: they belonged to the
            // configured tracker (the only one the app could hear then).
            if (saved.isNotEmpty()) _history.value = saved.map {
                if (it.deviceKey.isBlank())
                    it.copy(deviceKey = DeviceConfig.targetDeviceName)
                else it
            }
        }
        // The last diag survives restarts: shown until a new one arrives.
        DiagStore.load(app)?.let { (diag, at) ->
            _diag.value = diag
            _diagReceivedAt.value = at
        }
        _downlinks.value = DownlinkStore.load(app)
        purgeExpiredDownlinks()  // yesterday's receipts start empty
        // Beacon reception doubles as telemetry: EVERY parsed advertisement
        // lands in the history tagged with its own tracker (multi-tracker
        // reception) — the remembered one is just the primary card.
        viewModelScope.launch {
            scanner.frames.collect { hit ->
                hit.payload?.let {
                    addHistory(it, DataSourceFlags.BLE, hit.device.address)
                }
            }
        }
        // Trek peer relays (ICD §10.9): the peer's body is tagged under
        // deviceKey = origin-hex so it draws as its own track, and the
        // freshest relay per origin feeds the peer contact card.
        viewModelScope.launch {
            scanner.peerFrames.collect { hit ->
                val peer = hit.peer ?: return@collect
                _peerContacts.value = _peerContacts.value +
                    mapOf(peer.deviceKey to PeerContact(
                        originKey = peer.deviceKey,
                        seq = peer.seq,
                        payload = peer.payload,
                        rssi = hit.device.rssi,
                        relayMac = hit.device.address,
                        lastSeenMs = System.currentTimeMillis(),
                    ))
                addHistory(peer.payload, DataSourceFlags.BLE, peer.deviceKey)
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

    /** MainActivity.onPause: the always-on BLE pull stops when the app
     *  goes to background (battery policy) and re-arms in onAppResume. */
    fun stopScanOnPause() {
        appResumed = false
        scanner.stop()
    }

    // ===== history =====

    /** One received frame destined for the history: the payload, its
     *  timestamp, the feed it arrived on and the tracker identity. */
    data class HistoryFeed(
        val payload: MiniLoRaPayload,
        val ts: Long,
        val source: DataSourceFlags,
        val deviceKey: String,
    )

    private fun addHistory(payload: MiniLoRaPayload, source: DataSourceFlags,
                           deviceKey: String = "") {
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
            // The device key must match too — two trackers waking in the
            // same 10 s window are two cards, not one.
            val existing = _history.value.firstOrNull {
                it.deviceKey == deviceKey && Math.abs(it.timeUnix - ts) <= 10
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
                add(PayloadHistoryEntry(payload, ts, source, deviceKey))
                addAll(_history.value)
            }
        }
        // RAM cap: the file store keeps 400, but the in-memory list grew
        // unbounded — a long field day with continuous BLE scanning made
        // every frame O(n) to merge and the list ~300 B/entry. 1000
        // entries ≈ 10 days at a 15-min cycle for several trackers.
        if (_history.value.size > 1000) {
            _history.value = _history.value.take(1000)
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
    private fun mergeHistory(entries: List<HistoryFeed>) {
        if (entries.isEmpty()) return
        var list = _history.value
        val nowS = System.currentTimeMillis() / 1000
        for (feed in entries) {
            val payload = feed.payload
            val source = feed.source
            val deviceKey = feed.deviceKey
            val ts = when {
                payload.timeUnixSeconds in 1..(nowS + 600) ->
                    payload.timeUnixSeconds
                feed.ts > 0 -> feed.ts
                else -> nowS
            }
            val existing = list.firstOrNull {
                it.deviceKey == deviceKey && Math.abs(it.timeUnix - ts) <= 10
            }
            list = if (existing != null) {
                list.map {
                    if (it === existing) it.copy(source = it.source.combine(source))
                    else it
                }
            } else {
                (list + PayloadHistoryEntry(payload, ts, source, deviceKey))
                    .sortedByDescending { it.timeUnix }
            }
        }
        // RAM cap — same rationale as addHistory's.
        if (list.size > 1000) {
            list = list.take(1000)
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
    private fun setDiag(diag: MiniLoRaDiag, raw: ByteArray, receivedAtMs: Long,
                        deviceKey: String) {
        // The bootCount regression guard applies per device: tracker A at
        // boot 500 must not silence tracker B at boot 3.
        val cur = _diag.value
        if (cur != null && diag.bootCount < cur.bootCount &&
            _diagDeviceKey.value == deviceKey) return
        DiagStore.save(getApplication(), diag, raw, receivedAtMs, deviceKey)
        // display it only when it belongs to the selected tracker (or All)
        if (deviceKey == _selectedDeviceKey.value || _selectedDeviceKey.value.isBlank()) {
            _diag.value = diag
            _diagReceivedAt.value = receivedAtMs
            _diagDeviceKey.value = deviceKey
        }
    }

    /** Record a successfully queued downlink; a previous pending one is
     *  superseded (TTN's downlink/push replaces the class-A queue). */
    private fun recordDownlink(deviceKey: String, label: String, bytes: ByteArray) {
        val hex = bytes.joinToString(" ") { "%02X".format(it) }
        val now = System.currentTimeMillis()
        _downlinks.value = buildList {
            addAll(_downlinks.value.map {
                if (it.status == "pending" && it.deviceKey == deviceKey)
                    it.copy(status = "replaced") else it
            })
            add(DownlinkEntry(label, hex, now, "pending", deviceKey = deviceKey))
        }.takeLast(12)
        DownlinkStore.save(getApplication(), _downlinks.value)
    }

    /** Called after each pull: an uplink received at/after the queue time
     *  carried the pending frame in its RX window — mark it sent, stamped
     *  with THAT uplink's time (the honest delivery moment, not the queue
     *  time). Keyed per tracker: a pull for one device never flips
     *  another device's receipt. */
    private fun markDownlinksSent(deviceKey: String, newestUplinkSec: Long) {
        if (newestUplinkSec <= 0) return
        var changed = false
        _downlinks.value = _downlinks.value.map {
            if (it.status == "pending" && it.deviceKey == deviceKey &&
                newestUplinkSec * 1000 >= it.queuedAtMs) {
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
        val primary = DeviceConfig.ttnDeviceId.ifBlank { DeviceConfig.targetDeviceName }
        // Multi-tracker: the primary id plus every extra one from Settings;
        // each pull is tagged with its own identity.
        val ids = (listOf(primary) + DeviceConfig.ttnDeviceIds)
            .filter { it.isNotBlank() }.distinct()
        for (dev in ids) {
            // One storage-API call serves everything: the history backfill,
            // the latest diag, and the downlink-queue state (an uplink
            // received at/after the queue time carried the pending frame
            // in its RX window).
            val frames = TTNClient.fetchRecentFrames(
                DeviceConfig.ttnApiKey, DeviceConfig.ttnAppId, dev,
            ) ?: continue
            frames.maxOfOrNull { it.third }?.let { markDownlinksSent(dev, it) }
            frames.filter { it.first == 1 }.take(50).forEach { (_, b64, ts) ->
                decodeTTNPayload(b64)?.let {
                    mergeHistory(listOf(HistoryFeed(it, ts, DataSourceFlags.TTN, dev)))
                }
            }
            frames.firstOrNull { it.first == 2 }?.let { (_, b64, ts) ->
                base64ToBytes(b64)?.let { raw ->
                    MiniLoRaDiag.fromBytes(raw)?.let { diag ->
                        // Stamp the DEVICE's send time (TTN received_at ≈ the
                        // radio transmission), not the phone's pull time.
                        val sentAtMs = if (ts > 0) ts * 1000
                                       else System.currentTimeMillis()
                        setDiag(diag, raw, sentAtMs, dev)
                    }
                }
            }
        }
        fetchGroupMap()
    }

    /** identity -> display group, learned from the configured server's
     *  /api/devices ([device_groups]) — a tracker heard as a BLE MAC, a
     *  TTN id and a WiFi name then displays as ONE card. */
    private suspend fun fetchGroupMap() {
        if (!DeviceConfig.wifiConfigured()) return
        try {
            val rows = WiFiClient.fetchDevices(
                DeviceConfig.wifiServerIp, DeviceConfig.wifiServerPort)
            val map = rows.mapNotNull { r ->
                val g = r.group?.takeIf { it.isNotBlank() }
                        ?: return@mapNotNull null
                r.deviceId to g
            }.toMap()
            if (map.isNotEmpty() && map != DeviceConfig.deviceGroupMap) {
                DeviceConfig.deviceGroupMap = map
                DeviceConfig.save()
            }
        } catch (e: Exception) {
            Log.d("AppViewModel", "group map fetch skipped: ${e.message}")
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
        val wifiKey = dev   // WiFi-path frames belong to the push-name identity
        recent.maxOfOrNull { it.second }?.let { markDownlinksSent(wifiKey, it) }
        recent.forEach { (payload, seenAt) ->
            mergeHistory(listOf(HistoryFeed(payload, seenAt, DataSourceFlags.WIFI, wifiKey)))
        }
        WiFiClient.fetchLatestDiag(
            DeviceConfig.wifiServerIp, DeviceConfig.wifiServerPort, dev,
        )?.let { (diag, raw, seenAt) ->
            val sentAtMs = if (seenAt > 0) seenAt * 1000
                           else System.currentTimeMillis()
            setDiag(diag, raw, sentAtMs, wifiKey)
        }
        fetchGroupMap()
    }

    // ===== BLE beacon polling =====

    /**
     * BLE reception is ALWAYS-ON while the app is open: the tracker
     * beacons its telemetry whether or not any other feed is configured,
     * so the phone keeps a BALANCED scan for the whole foreground time
     * (started in onAppResume / the poll loop, stopped in onPause —
     * battery-cheap, no duty-cycling). Hits land in the history through
     * the frames collector, tagged with their own tracker identity. The
     * on/off switch is persisted (DeviceConfig.blePollingEnabled, default
     * on) and auto-resumed from init().
     */
    fun toggleBLEPolling() {
        if (_blePolling.value) {
            bleJob?.cancel(); bleJob = null
            scanner.stop()
            _blePolling.value = false
            DeviceConfig.blePollingEnabled = false
            DeviceConfig.save()
            toast("BLE reception stopped")
        } else {
            _blePolling.value = true
            DeviceConfig.blePollingEnabled = true
            DeviceConfig.save()
            bleJob = viewModelScope.launch { blePollLoop() }
            scanner.start(DeviceConfig.targetDeviceName,
                          DeviceConfig.targetMacAddress,
                          balanced = true) { /* transient */ }
            toast("BLE reception on — every MiniLoRa beacon in range")
        }
    }

    private suspend fun blePollLoop() {
        // Continuous BALANCED scan while the app is open ("BLE pulling
        // active as long as the app is open") — stopped onPause, re-armed
        // onResume. The frames flow feeds the history for every tracker.
        while (true) {
            if (appResumed && !scanActive.value) {
                scanner.start(DeviceConfig.targetDeviceName,
                              DeviceConfig.targetMacAddress,
                              balanced = true) { /* transient */ }
            }
            // Devices tab hygiene: drop trackers not heard for 60 s.
            scanner.expireStale()
            delay(15_000)
        }
    }

    private var appResumed = false

    /** MainActivity.onResume: BLE pulling is active whenever the app is
     *  open (the balanced scan starts here, stops in onPause). */
    fun onAppResume() {
        appResumed = true
        if (DeviceConfig.blePollingEnabled && !scanActive.value) {
            scanner.start(DeviceConfig.targetDeviceName,
                          DeviceConfig.targetMacAddress,
                          balanced = true) { /* transient */ }
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
            val target = DeviceConfig.ttnDeviceId.ifBlank { DeviceConfig.targetDeviceName }
            val ok = TTNClient.sendDownlink(
                DeviceConfig.ttnApiKey, DeviceConfig.ttnAppId,
                target,
                fPort = 3, bytes = bytes,
            )
            if (ok) recordDownlink(target, label, bytes)
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
            val target = DeviceConfig.ttnDeviceId.ifBlank { DeviceConfig.targetDeviceName }
            val ok = TTNClient.sendDownlink(
                DeviceConfig.ttnApiKey, DeviceConfig.ttnAppId,
                target,
                fPort = 3, bytes = bytes,
            )
            if (ok) recordDownlink(target, label, bytes)
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
