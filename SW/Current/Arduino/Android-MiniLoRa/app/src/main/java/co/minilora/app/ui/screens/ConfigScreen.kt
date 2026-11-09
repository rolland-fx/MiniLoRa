package co.minilora.app.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import co.minilora.app.data.model.MiniLoRaCommands
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import co.minilora.app.ui.AppViewModel
import co.minilora.app.util.DeviceConfig
import kotlinx.coroutines.launch

/** Sleep table labels (index 0..8) — shared with the remote tab. */
private val SLEEP_LABELS = MiniLoRaCommands.SLEEP_LABELS

/**
 * Device configuration editor (ICD §4). Writes go through the CRC-protected
 * v3 envelope; "Read" shows the device's live status + current config first.
 */
@Composable
fun ConfigScreen(vm: AppViewModel, modifier: Modifier = Modifier) {
    var globalName by remember { mutableStateOf(DeviceConfig.deviceGlobalName) }
    var wifiSSID by remember { mutableStateOf(DeviceConfig.deviceWifiSSID) }
    var wifiPassword by remember { mutableStateOf(DeviceConfig.deviceWifiPassword) }
    var wifiIP by remember { mutableStateOf(DeviceConfig.deviceWifiServerIP) }
    var wifiPort by remember { mutableStateOf(DeviceConfig.deviceWifiServerPort.toString()) }
    var joinEUI by remember { mutableStateOf(DeviceConfig.deviceJoinEUI) }
    var devEUI by remember { mutableStateOf(DeviceConfig.deviceDevEUI) }
    var appKey by remember { mutableStateOf(DeviceConfig.deviceAppKey) }
    var nwkKey by remember { mutableStateOf(DeviceConfig.deviceNwkKey) }
    var cfgLoRaWAN by remember { mutableStateOf(DeviceConfig.deviceCfgLoRaWAN) }
    var cfgWiFi by remember { mutableStateOf(DeviceConfig.deviceCfgWiFi) }
    var cfgGNSS by remember { mutableStateOf(DeviceConfig.deviceCfgGNSS) }
    var cfgBME280 by remember { mutableStateOf(DeviceConfig.deviceCfgBME280) }
    var cfgOPT30 by remember { mutableStateOf(DeviceConfig.deviceCfgOPT30) }
    var cfgLIS3DH by remember { mutableStateOf(DeviceConfig.deviceCfgLIS3DH) }
    var cfgBLE by remember { mutableStateOf(DeviceConfig.deviceCfgBLE) }
    var cfgZigbee by remember { mutableStateOf(DeviceConfig.deviceCfgZigbee) }
    var cfgRange by remember { mutableStateOf(DeviceConfig.deviceCfgRange.toFloat()) }
    var cfgAssetMode by remember { mutableStateOf(DeviceConfig.deviceCfgAssetMode) }
    var cfgMotionSens by remember { mutableStateOf(DeviceConfig.deviceCfgMotionSens) }
    var cfgAssetHeartbeatH by remember { mutableStateOf(DeviceConfig.deviceCfgAssetHeartbeatH.toString()) }
    var cfgBurstIntervalMin by remember { mutableStateOf(DeviceConfig.deviceCfgBurstIntervalMin.toString()) }
    var cfgBurstMaxPerDay by remember { mutableStateOf(DeviceConfig.deviceCfgBurstMaxPerDay.toString()) }
    // P2P star (ICD §10) — provisioned via the schema-5 envelope
    var cfgP2P by remember { mutableStateOf(false) }
    var cfgP2PRole by remember { mutableStateOf(false) }
    var cfgP2PProfile by remember { mutableStateOf(0) }
    var cfgP2PRxAfterTx by remember { mutableStateOf(true) }
    var cfgP2PShortId by remember { mutableStateOf("") }
    var cfgMeshKey by remember { mutableStateOf("") }
    var cfgP2PTxPower by remember { mutableStateOf(DeviceConfig.deviceCfgP2PTxPower.toString()) }

    val status by vm.statusFrame.collectAsState()
    val busy by vm.bleBusy.collectAsState()
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    var message by remember { mutableStateOf("") }

    fun persist() {
        DeviceConfig.deviceGlobalName = globalName.trim()
        DeviceConfig.deviceWifiSSID = wifiSSID.trim()
        DeviceConfig.deviceWifiPassword = wifiPassword
        DeviceConfig.deviceWifiServerIP = wifiIP.trim()
        DeviceConfig.deviceWifiServerPort = wifiPort.toIntOrNull() ?: 0
        DeviceConfig.deviceJoinEUI = joinEUI.trim()
        DeviceConfig.deviceDevEUI = devEUI.trim()
        DeviceConfig.deviceAppKey = appKey.trim()
        DeviceConfig.deviceNwkKey = nwkKey.trim()
        DeviceConfig.deviceCfgLoRaWAN = cfgLoRaWAN
        DeviceConfig.deviceCfgWiFi = cfgWiFi
        DeviceConfig.deviceCfgGNSS = cfgGNSS
        DeviceConfig.deviceCfgBME280 = cfgBME280
        DeviceConfig.deviceCfgOPT30 = cfgOPT30
        DeviceConfig.deviceCfgLIS3DH = cfgLIS3DH
        DeviceConfig.deviceCfgBLE = cfgBLE
        DeviceConfig.deviceCfgZigbee = cfgZigbee
        DeviceConfig.deviceCfgRange = cfgRange.toInt()
        DeviceConfig.deviceCfgAssetMode = cfgAssetMode
        DeviceConfig.deviceCfgMotionSens = cfgMotionSens
        DeviceConfig.deviceCfgAssetHeartbeatH = cfgAssetHeartbeatH.toIntOrNull() ?: 24
        DeviceConfig.deviceCfgBurstIntervalMin = cfgBurstIntervalMin.toIntOrNull() ?: 5
        DeviceConfig.deviceCfgBurstMaxPerDay = cfgBurstMaxPerDay.toIntOrNull() ?: 20
        // P2P (§10): kept in DeviceConfig so the editor survives restarts
        DeviceConfig.deviceCfgP2P = cfgP2P
        DeviceConfig.deviceCfgP2PGateway = cfgP2PRole
        DeviceConfig.deviceCfgP2PProfile = cfgP2PProfile
        DeviceConfig.deviceCfgP2PRxAfterTx = cfgP2PRxAfterTx
        DeviceConfig.deviceCfgP2PShortId = cfgP2PShortId.trim()
        DeviceConfig.deviceCfgMeshKey = cfgMeshKey.trim()
        DeviceConfig.deviceCfgP2PTxPower = cfgP2PTxPower.toIntOrNull() ?: 0
        DeviceConfig.save()
    }

    // ---- config file export / import (ICD §4 envelopes, .cfg) ----
    // The envelope is the device's native CRC-protected wire format: export
    // saves exactly what would be written; import parses + CRC-validates and
    // fills the editor — nothing is written to the device until the user
    // presses "Write to device".

    /** Editor state → device wire envelope (same bytes "Write to device" sends). */
    fun buildCurrentEnvelope(): ByteArray =
        co.minilora.app.ble.BLEDeviceConfigurator.buildEnvelope(
            co.minilora.app.ble.BLEDeviceConfigurator.buildBody(
                globalName.trim(), wifiSSID.trim(), wifiPassword,
                parseIP(wifiIP.trim()), wifiPort.toIntOrNull() ?: 0,
                hexToLong(joinEUI), hexToLong(devEUI),
                hexToBytes(appKey, 16), hexToBytes(nwkKey, 16),
                cfgLoRaWAN, cfgWiFi, cfgGNSS, cfgBME280,
                cfgOPT30, cfgLIS3DH, cfgBLE, cfgZigbee,
                cfgRange.toInt(),
                cfgAssetMode, cfgMotionSens,
                cfgAssetHeartbeatH.toIntOrNull() ?: 24,
                cfgBurstIntervalMin.toIntOrNull() ?: 5,
                cfgBurstMaxPerDay.toIntOrNull() ?: 20,
                cfgP2P = cfgP2P,
                cfgP2PRole = cfgP2PRole,
                cfgP2PProfile = cfgP2PProfile,
                cfgP2PRxAfterTx = cfgP2PRxAfterTx,
                cfgP2PShortId = cfgP2PShortId.trim().hexToLong(),
                cfgMeshKey = hexToBytes(cfgMeshKey, 16),
                cfgP2PTxPower = cfgP2PTxPower.toIntOrNull() ?: 0,
            ))

    /** Parsed device config (or .cfg file) → editor fields. */
    fun applyConfig(cfg: Map<String, Any>) {
        globalName = cfg["globalName"] as? String ?: globalName
        wifiSSID = cfg["wifiSSID"] as? String ?: wifiSSID
        wifiPassword = cfg["wifiPassword"] as? String ?: wifiPassword
        (cfg["wifiServerIP"] as? Long)?.let { ip ->
            wifiIP = listOf(
                ((ip shr 24) and 0xFF).toInt(),
                ((ip shr 16) and 0xFF).toInt(),
                ((ip shr 8) and 0xFF).toInt(),
                (ip and 0xFF).toInt(),
            ).joinToString(".")
        }
        (cfg["wifiServerPort"] as? Int)?.let { wifiPort = it.toString() }
        (cfg["joinEUI"] as? Long)?.let { joinEUI = "%016X".format(it) }
        (cfg["devEUI"] as? Long)?.let { devEUI = "%016X".format(it) }
        (cfg["appKey"] as? String)?.let { appKey = it }
        (cfg["nwkKey"] as? String)?.let { nwkKey = it }
        cfgLoRaWAN = cfg["cfgLoRaWAN"] == true
        cfgWiFi = cfg["cfgWiFi"] == true
        cfgGNSS = cfg["cfgGNSS"] == true
        cfgBME280 = cfg["cfgBME280"] == true
        cfgOPT30 = cfg["cfgOPT30"] == true
        cfgLIS3DH = cfg["cfgLIS3DH"] == true
        cfgBLE = cfg["cfgBLE"] == true
        cfgZigbee = cfg["cfgZigbee"] == true
        (cfg["cfgRange"] as? Int)?.let { cfgRange = it.toFloat() }
        cfg["cfgAssetMode"]?.let { cfgAssetMode = it == true }
        (cfg["cfgMotionSens"] as? Int)?.let { cfgMotionSens = it }
        (cfg["cfgAssetHeartbeatH"] as? Int)?.let { cfgAssetHeartbeatH = it.toString() }
        (cfg["cfgBurstIntervalMin"] as? Int)?.let { cfgBurstIntervalMin = it.toString() }
        (cfg["cfgBurstMaxPerDay"] as? Int)?.let { cfgBurstMaxPerDay = it.toString() }
        // P2P: absent from pre-schema-5 bodies → editor values kept
        cfgP2P = cfg["cfgLoRaMode"] == true
        cfgP2PRole = cfg["cfgP2PRole"] == true
        (cfg["cfgP2PProfile"] as? Int)?.let { cfgP2PProfile = it }
        cfg["cfgP2PRxAfterTx"]?.let { cfgP2PRxAfterTx = it == true }
        (cfg["cfgP2PShortId"] as? Long)?.let {
            cfgP2PShortId = if (it == 0L) "" else "%08X".format(it)
        }
        (cfg["cfgMeshKey"] as? String)?.let { if (it.any { c -> c != '0' }) cfgMeshKey = it }
        (cfg["cfgP2PTxPower"] as? Int)?.let { cfgP2PTxPower = it.toString() }
    }

    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri ->
        if (uri != null) {
            scope.launch {
                message = try {
                    persist()
                    val env = buildCurrentEnvelope()
                    context.contentResolver.openOutputStream(uri)?.use {
                        it.write(env)
                    } ?: error("Cannot open ${uri.lastPathSegment}")
                    "Config exported (${env.size} B)"
                } catch (e: Exception) {
                    "Export failed: ${e.message}"
                }
            }
        }
    }

    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            scope.launch {
                message = try {
                    val bytes = context.contentResolver.openInputStream(uri)
                        ?.use { it.readBytes() } ?: error("File is empty")
                    val cfg = vm.parseConfigBytes(bytes)
                        ?: error("Not a valid MiniLoRa config (size or CRC)")
                    applyConfig(cfg)
                    persist()
                    "Config loaded — review, then Write to device"
                } catch (e: Exception) {
                    "Import failed: ${e.message}"
                }
            }
        }
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        OutlinedTextField(globalName, { globalName = it },
            label = { Text("Device name") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(wifiSSID, { wifiSSID = it },
            label = { Text("WiFi SSID") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(wifiPassword, { wifiPassword = it },
            label = { Text("WiFi password") }, modifier = Modifier.fillMaxWidth())
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            OutlinedTextField(wifiIP, { wifiIP = it },
                label = { Text("Server IP") }, modifier = Modifier.weight(2f))
            OutlinedTextField(wifiPort, { wifiPort = it },
                label = { Text("Port") }, modifier = Modifier.weight(1f))
        }
        OutlinedTextField(joinEUI, { joinEUI = it },
            label = { Text("JoinEUI (hex)") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(devEUI, { devEUI = it },
            label = { Text("DevEUI (hex)") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(appKey, { appKey = it },
            label = { Text("AppKey (hex, 32)") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(nwkKey, { nwkKey = it },
            label = { Text("NwkKey (hex, 32)") }, modifier = Modifier.fillMaxWidth())

        Text("Sensors & transports", style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(top = 6.dp))
        listOf(
            "LoRaWAN" to cfgLoRaWAN, "WiFi" to cfgWiFi, "GNSS" to cfgGNSS,
            "BME280" to cfgBME280, "OPT3001" to cfgOPT30, "LIS3DH" to cfgLIS3DH,
            "BLE beacon" to cfgBLE, "Zigbee" to cfgZigbee,
        ).forEachIndexed { idx, (label, checked) ->
            Row(Modifier.fillMaxWidth(), verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
                val setter: (Boolean) -> Unit = when (idx) {
                    0 -> { v -> cfgLoRaWAN = v }
                    1 -> { v -> cfgWiFi = v }
                    2 -> { v -> cfgGNSS = v }
                    3 -> { v -> cfgBME280 = v }
                    4 -> { v -> cfgOPT30 = v }
                    5 -> { v -> cfgLIS3DH = v }
                    6 -> { v -> cfgBLE = v }
                    else -> { v -> cfgZigbee = v }
                }
                Checkbox(checked = checked, onCheckedChange = setter)
            }
        }

        Text("Sleep duration: ${SLEEP_LABELS[cfgRange.toInt().coerceIn(0, 8)]}",
            style = MaterialTheme.typography.bodyMedium)
        Slider(
            value = cfgRange,
            onValueChange = { cfgRange = it },
            valueRange = 0f..8f,
            steps = 7,
        )

        Text("Asset mode (P5)", style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(top = 6.dp))
        Row(Modifier.fillMaxWidth(),
            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("Wake on motion (uses the accelerometer)",
                Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            Checkbox(checked = cfgAssetMode, onCheckedChange = { cfgAssetMode = it })
        }
        // ---- P2P star (ICD §10) ----
        Text("P2P star (no TTN — LoRa raw, needs a gateway device)",
            style = MaterialTheme.typography.titleMedium,
            modifier = Modifier.padding(top = 8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Text("P2P mode", style = MaterialTheme.typography.bodyMedium)
                Checkbox(checked = cfgP2P, onCheckedChange = { cfgP2P = it })
            }
            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Text("Gateway role", style = MaterialTheme.typography.bodyMedium)
                Checkbox(checked = cfgP2PRole, onCheckedChange = { cfgP2PRole = it })
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf("Long-range SF12", "Balanced SF10", "Bench SF7")
                .forEachIndexed { i, label ->
                    OutlinedButton(onClick = { cfgP2PProfile = i },
                        enabled = cfgP2P) {
                        Text(label, style = MaterialTheme.typography.labelSmall,
                             color = if (cfgP2PProfile == i)
                                 MaterialTheme.colorScheme.primary
                                 else MaterialTheme.colorScheme.onSurface)
                    }
                }
        }
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("Command window after each TX (recommended)",
                Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            Checkbox(checked = cfgP2PRxAfterTx,
                     onCheckedChange = { cfgP2PRxAfterTx = it }, enabled = cfgP2P)
        }
        OutlinedTextField(cfgP2PShortId, { cfgP2PShortId = it },
            label = { Text("Short id hex (empty = from DevEUI)") },
            enabled = cfgP2P, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(cfgMeshKey, { cfgMeshKey = it },
            label = { Text("Mesh key — 32 hex chars (required for P2P)") },
            enabled = cfgP2P, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(cfgP2PTxPower, { cfgP2PTxPower = it },
            label = { Text("TX power dBm (0 = 14; up to 22 — legal on 869.525)") },
            enabled = cfgP2P, modifier = Modifier.fillMaxWidth())
        if (cfgAssetMode) {
            Text("Motion sensitivity", style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                listOf("Low", "Med", "High").forEachIndexed { i, label ->
                    val sens = i + 1
                    OutlinedButton(onClick = { cfgMotionSens = sens },
                        enabled = cfgMotionSens != sens,
                        modifier = Modifier.weight(1f)) { Text(label) }
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                OutlinedTextField(cfgAssetHeartbeatH, { cfgAssetHeartbeatH = it },
                    label = { Text("Heartbeat (h)") }, modifier = Modifier.weight(1f))
                OutlinedTextField(cfgBurstIntervalMin, { cfgBurstIntervalMin = it },
                    label = { Text("Burst (min)") }, modifier = Modifier.weight(1f))
                OutlinedTextField(cfgBurstMaxPerDay, { cfgBurstMaxPerDay = it },
                    label = { Text("Bursts/day") }, modifier = Modifier.weight(1f))
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier.padding(vertical = 8.dp)) {
            OutlinedButton(
                onClick = {
                    persist()
                    scope.launch {
                        message = "Reading device…"
                        val result = vm.readDeviceStatusAndConfig()
                        message = if (result == null) "Read failed"
                                  else "Read OK" + (result.first?.get("fwVersion")?.let { " (fw $it)" } ?: "")
                        // Populate the editor from the device's current config
                        result?.second?.let { cfg -> applyConfig(cfg) }
                    }
                },
                enabled = !busy,
            ) { Text("Read") }
            Button(
                onClick = {
                    persist()
                    scope.launch {
                        message = "Writing (CRC-protected)…"
                        vm.writeConfig(buildCurrentEnvelope())
                        message = "Write sent"
                    }
                },
                enabled = !busy,
            ) { Text("Write to device") }
        }
        // ---- config file export / import ----
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = {
                    exportLauncher.launch(
                        "minilora-" +
                            globalName.trim().ifBlank { "device" } + ".cfg")
                },
                enabled = !busy,
            ) { Text("Export .cfg") }
            OutlinedButton(
                onClick = { importLauncher.launch(arrayOf("*/*")) },
                enabled = !busy,
            ) { Text("Import .cfg") }
        }
        if (message.isNotEmpty()) {
            Text(message, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        status?.let {
            Text(
                "Device: bat ${it["batMv"]} mV · boots ${it["bootCount"]} · " +
                    "gpsFails ${it["gpsConsecFails"]} · joined=${it["joined"]}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

// ---- field helpers (ported from the old fragment) ----

private fun parseIP(ip: String): Long {
    val parts = ip.split(".")
    if (parts.size != 4) return 0L
    var result = 0L
    for (i in 0..3) {
        val octet = parts[i].toIntOrNull()?.coerceIn(0, 255) ?: return 0L
        result = result or (octet.toLong() shl ((3 - i) * 8))
    }
    return result
}

private fun hexToLong(hex: String): Long =
    hex.filterNot { it.isWhitespace() }.toLongOrNull(16) ?: 0L

private fun hexToBytes(hex: String, expected: Int): ByteArray {
    val clean = hex.filterNot { it.isWhitespace() }
    if (clean.isEmpty()) return ByteArray(expected)
    return try {
        val bytes = ByteArray(clean.length / 2) {
            clean.substring(it * 2, it * 2 + 2).toInt(16).toByte()
        }
        if (bytes.size == expected) bytes else bytes.copyOf(expected)
    } catch (_: Exception) {
        ByteArray(expected)
    }
}
