package co.minilora.app.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.util.Log
import co.minilora.app.data.model.MiniLoRaPayloadParser
import co.minilora.app.ui.DeviceItem
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * BLE scan for MiniLoRa beacon frames. Extracted verbatim from the old
 * MainActivity: the scanner parses every advertisement as a payload v1
 * frame, so only devices that beacon a valid MiniLoRa frame are listed
 * (this doubles as the telemetry beacon receiver).
 */
class BleScanner(private val adapter: BluetoothAdapter?) {

    data class Hit(val device: DeviceItem,
                   val payload: co.minilora.app.data.model.MiniLoRaPayload?,
                   val peer: co.minilora.app.data.model.PeerRelayFrame? = null)

    private val _hits = MutableStateFlow<List<Hit>>(emptyList())
    val hits: StateFlow<List<Hit>> = _hits

    private val _scanning = MutableStateFlow(false)
    val scanning: StateFlow<Boolean> = _scanning

    private val _lastPayload = MutableStateFlow<Hit?>(null)
    val lastPayload: StateFlow<Hit?> = _lastPayload

    /** Every parsed beacon frame, from ANY MiniLoRa tracker — the
     * multi-tracker feed (each Hit carries the advertising MAC). */
    private val _frames = MutableSharedFlow<Hit>(extraBufferCapacity = 64)
    val frames: MutableSharedFlow<Hit> = _frames

    /** Trek peer-relay advertisements (ICD §10.9): the tracker's peer's
     * last frame, relayed so both phones can draw both tracks. */
    private val _peerFrames = MutableSharedFlow<Hit>(extraBufferCapacity = 64)
    val peerFrames: MutableSharedFlow<Hit> = _peerFrames

    private var scanner: android.bluetooth.le.BluetoothLeScanner? = null
    private val callbacks = mutableListOf<ScanCallback>()
    private val lastProcessedTime = HashMap<String, Long>()

    /** The config window advertises this service instead of a beacon frame. */
    private val configServiceUuid = java.util.UUID.fromString(
        "0000FACE-0000-1000-8000-00805F9B34FB")

    @SuppressLint("MissingPermission")
    fun start(
        targetName: String, targetMac: String,
        balanced: Boolean = false,
        onError: (String) -> Unit,
    ) {
        if (_scanning.value) return
        try {
            scanner = adapter?.bluetoothLeScanner ?: run {
                onError("No BLE scanner available")
                return
            }
            // TWO registrations: setLegacy(false) reports ONLY extended
            // advertisements (the tracker's beacon is extended, fine) but
            // silently DROPS the legacy connectable advertisement the
            // config window uses - the device was invisible exactly when
            // the user needed to provision it. Scan both kinds, with
            // DISTINCT callbacks (Android rejects a reused callback with
            // SCAN_ERROR_ALREADY_STARTED).
            val mkCb = {
                object : ScanCallback() {
                    override fun onScanResult(callbackType: Int, result: ScanResult) {
                        runCatching { process(result, targetName, targetMac) }
                    }

                    override fun onBatchScanResults(results: List<ScanResult>) {
                        for (r in results) runCatching { process(r, targetName, targetMac) }
                    }

                    override fun onScanFailed(errorCode: Int) {
                        onError("Scan error: $errorCode")
                        _scanning.value = false
                    }
                }
            }
            val base = ScanSettings.Builder()
                .setScanMode(if (balanced) ScanSettings.SCAN_MODE_BALANCED
                             else ScanSettings.SCAN_MODE_LOW_LATENCY)
                .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
                .setPhy(ScanSettings.PHY_LE_ALL_SUPPORTED)
            val cbLegacy = mkCb()
            val cbExt = mkCb()
            scanner?.startScan(
                listOf(ScanFilter.Builder().build()),
                base.setLegacy(true).build(),   // legacy advs (config window)
                cbLegacy,
            )
            scanner?.startScan(
                listOf(ScanFilter.Builder().build()),
                base.setLegacy(false).build(),  // extended advs (beacon frames)
                cbExt,
            )
            callbacks.clear()
            callbacks.addAll(listOf(cbLegacy, cbExt))
            _scanning.value = true
        } catch (e: SecurityException) {
            onError("BLE scan failed: ${e.message}")
            _scanning.value = false
        } catch (e: Exception) {
            onError("Unable to start scanning: ${e.message}")
            _scanning.value = false
        }
    }

    fun stop() {
        for (cb in callbacks) {
            try {
                scanner?.stopScan(cb)
            } catch (_: Exception) {
            }
        }
        callbacks.clear()
        _scanning.value = false
    }

    @SuppressLint("MissingPermission")
    private fun process(result: ScanResult, targetName: String, targetMac: String) {
        val record = result.scanRecord ?: return
        val packet = record.getBytes()
        if (packet.isEmpty()) return
        val device = result.device
        val address = device.address
        val now = System.currentTimeMillis()
        // Debounce keyed by (MAC, packet size): the relay device advertises
        // its OWN beacon (30 B mfg) and the peer relay (40 B mfg) on the
        // same MAC within one interval — a plain MAC debounce would drop
        // one of the two every time.
        val pktKey = "$address:${packet.size}"
        lastProcessedTime[pktKey]?.let { if (now - it < DEBOUNCE_MS) return }
        lastProcessedTime[pktKey] = now

        // Debug visibility: one line per device per 500 ms (the debounce
        // above gates it) — shows whether ANY advertisement arrives and
        // what it carries, before any filtering decision.
        Log.d("BleScanner",
            "adv $address devName='${device.name}' advName='${record.deviceName}' " +
                "svc=${record.serviceUuids} bytes=${packet.size}")

        val name = (record.deviceName ?: device.name ?: "Unknown")
            .replace(Regex("[\\x00-\\x1F\\x7F]"), "").trim()
        val nameMatch = targetName.isEmpty() || name.contains(targetName, ignoreCase = true)
        val macMatch = targetMac.isEmpty() || address.equals(targetMac, ignoreCase = true)
        // NO hard filter: every MiniLoRa beacon is parsed and tagged with
        // its own MAC (multi-tracker reception). The remembered tracker
        // only decides which hit lands in lastPayload (the primary card).

        // Three acceptance paths: a peer-relay advertisement (trek §10.9),
        // a valid MiniLoRa beacon frame (telemetry beacon), OR the
        // config-window advertisement (name + config service UUID, no
        // payload). Before the second path existed the scanner silently
        // dropped the device exactly when the user needed it most - during
        // provisioning.
        val isConfigAdv = record.serviceUuids?.any {
            it.uuid == configServiceUuid
        } == true
        val peer = MiniLoRaPayloadParser.peerFromBytes(packet)
        val payload = MiniLoRaPayloadParser.fromBytes(packet)
        if (peer == null && payload == null) return
        Log.d("BleScanner",
            "hit $address name='$name' rssi=${result.rssi} " +
                "configAdv=$isConfigAdv payload=${payload != null} " +
                "peerRelay=${peer?.deviceKey}")
        val hit = Hit(
            DeviceItem(name = name, address = address,
                rssi = result.rssi, discoveredAt = now),
            payload,
            peer,
        )
        // One card per device: the trek relay advertises its own beacon AND
        // the peer relay on the same MAC — the hit keeps the freshest of each.
        _hits.value = (listOf(hit) + _hits.value.filterNot {
            it.device.address == address
        }).groupBy { it.device.address }.map { (_, group) ->
            group.reduce { a, b -> a.copy(payload = a.payload ?: b.payload,
                                          peer = a.peer ?: b.peer) }
        }.sortedByDescending { it.device.rssi }
        if (nameMatch || macMatch) _lastPayload.value = hit
        if (peer != null) _peerFrames.tryEmit(hit)
        if (payload != null) _frames.tryEmit(hit)
    }

    companion object {
        private const val DEBOUNCE_MS = 500L
    }
}
