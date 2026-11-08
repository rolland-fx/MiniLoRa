package co.minilora.app.ui.screens

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import co.minilora.app.ui.AppViewModel
import co.minilora.app.util.DeviceConfig
import kotlinx.coroutines.launch

/** App settings: TTN API, local WiFi server, poll intervals. */
@Composable
fun SettingsScreen(vm: AppViewModel, onDone: () -> Unit, modifier: Modifier = Modifier) {
    var ttnApiKey by remember { mutableStateOf(DeviceConfig.ttnApiKey) }
    var ttnAppId by remember { mutableStateOf(DeviceConfig.ttnAppId) }
    var ttnDeviceId by remember { mutableStateOf(DeviceConfig.ttnDeviceId) }
    var ttnRegion by remember { mutableStateOf(DeviceConfig.ttnRegion) }
    var ttnInterval by remember { mutableStateOf(DeviceConfig.ttnPollIntervalSeconds.toString()) }
    var wifiServer by remember { mutableStateOf(DeviceConfig.wifiServerIp) }
    var wifiPort by remember { mutableStateOf(DeviceConfig.wifiServerPort.toString()) }
    var wifiInterval by remember { mutableStateOf(DeviceConfig.wifiPollIntervalSeconds.toString()) }
    var bleInterval by remember { mutableStateOf(DeviceConfig.blePollIntervalSeconds.toString()) }
    var targetName by remember { mutableStateOf(DeviceConfig.targetDeviceName) }
    var targetMac by remember { mutableStateOf(DeviceConfig.targetMacAddress) }
    val context = LocalContext.current
    val scope = androidx.compose.runtime.rememberCoroutineScope()
    var fileMsg by remember { mutableStateOf("") }

    // Push the editor fields into DeviceConfig (no navigation).
    fun persist() {
        DeviceConfig.ttnApiKey = ttnApiKey.trim()
        DeviceConfig.ttnAppId = ttnAppId.trim()
        DeviceConfig.ttnDeviceId = ttnDeviceId.trim()
        DeviceConfig.ttnRegion = ttnRegion.trim()
        DeviceConfig.ttnPollIntervalSeconds = ttnInterval.toIntOrNull() ?: 300
        DeviceConfig.wifiServerIp = wifiServer.trim()
        DeviceConfig.wifiServerPort = wifiPort.toIntOrNull()?.coerceIn(1, 65535) ?: 5000
        DeviceConfig.wifiPollIntervalSeconds = wifiInterval.toIntOrNull() ?: 300
        DeviceConfig.blePollIntervalSeconds =
            (bleInterval.toIntOrNull() ?: 60).coerceAtLeast(30)
        DeviceConfig.targetDeviceName = targetName.trim()
        DeviceConfig.targetMacAddress = targetMac.trim()
        DeviceConfig.save()
    }

    // Reload the editor from DeviceConfig (after a file import).
    fun reload() {
        ttnApiKey = DeviceConfig.ttnApiKey
        ttnAppId = DeviceConfig.ttnAppId
        ttnDeviceId = DeviceConfig.ttnDeviceId
        ttnRegion = DeviceConfig.ttnRegion
        ttnInterval = DeviceConfig.ttnPollIntervalSeconds.toString()
        wifiServer = DeviceConfig.wifiServerIp
        wifiPort = DeviceConfig.wifiServerPort.toString()
        wifiInterval = DeviceConfig.wifiPollIntervalSeconds.toString()
        bleInterval = DeviceConfig.blePollIntervalSeconds.toString()
        targetName = DeviceConfig.targetDeviceName
        targetMac = DeviceConfig.targetMacAddress
    }

    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/json")
    ) { uri: Uri? ->
        if (uri != null) {
            scope.launch {
                fileMsg = try {
                    persist()
                    val json = DeviceConfig.exportSettingsJson()
                    context.contentResolver.openOutputStream(uri)?.use {
                        it.write(json.toByteArray())
                    } ?: error("Cannot open ${uri.lastPathSegment}")
                    "Settings exported"
                } catch (e: Exception) {
                    "Export failed: ${e.message}"
                }
            }
        }
    }

    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null) {
            scope.launch {
                fileMsg = try {
                    val json = context.contentResolver.openInputStream(uri)
                        ?.use { it.readBytes().decodeToString() }
                        ?: error("File is empty")
                    DeviceConfig.importSettingsJson(json)
                    reload()
                    "Settings imported and saved"
                } catch (e: Exception) {
                    "Import failed: ${e.message}"
                }
            }
        }
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .systemBarsPadding()   // rendered standalone (no Scaffold): keep
            .imePadding()          // Save/Cancel clear of nav bar + keyboard
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text("Polling", style = MaterialTheme.typography.titleMedium,
            modifier = Modifier.padding(top = 8.dp))
        Text("How this phone talks to the tracker. Remembered across restarts.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("TTN polling", style = MaterialTheme.typography.bodyLarge)
            Switch(checked = vm.ttnPolling.collectAsState().value,
                   onCheckedChange = { vm.toggleTTNPolling() })
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("WiFi server polling", style = MaterialTheme.typography.bodyLarge)
            Switch(checked = vm.wifiPolling.collectAsState().value,
                   onCheckedChange = { vm.toggleWiFiPolling() })
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("BLE polling", style = MaterialTheme.typography.bodyLarge)
            Switch(checked = vm.blePolling.collectAsState().value,
                   onCheckedChange = { vm.toggleBLEPolling() })
        }

        Text("The Things Network", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(ttnApiKey, { ttnApiKey = it },
            label = { Text("API key (NNSXS…)") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(ttnAppId, { ttnAppId = it },
            label = { Text("Application ID") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(ttnDeviceId, { ttnDeviceId = it },
            label = { Text("Device ID") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(ttnRegion, { ttnRegion = it },
            label = { Text("Region (eu1, nam1, …)") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(ttnInterval, { ttnInterval = it },
            label = { Text("Poll interval (seconds)") }, modifier = Modifier.fillMaxWidth())

        Text("Local WiFi server", style = MaterialTheme.typography.titleMedium,
            modifier = Modifier.padding(top = 8.dp))
        OutlinedTextField(wifiServer, { wifiServer = it },
            label = { Text("Server IP") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(wifiPort, { wifiPort = it },
            label = { Text("Server port (default 5000)") },
            modifier = Modifier.fillMaxWidth())
        OutlinedTextField(wifiInterval, { wifiInterval = it },
            label = { Text("Poll interval (seconds)") }, modifier = Modifier.fillMaxWidth())

        Text("BLE beacon polling", style = MaterialTheme.typography.titleMedium,
            modifier = Modifier.padding(top = 8.dp))
        OutlinedTextField(bleInterval, { bleInterval = it },
            label = { Text("Poll interval (seconds, min 30)") },
            modifier = Modifier.fillMaxWidth())

        Text("Target device", style = MaterialTheme.typography.titleMedium,
            modifier = Modifier.padding(top = 8.dp))
        OutlinedTextField(targetName, { targetName = it },
            label = { Text("Name filter (BLE scan)") }, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(targetMac, { targetMac = it },
            label = { Text("MAC address (config/status target)") },
            modifier = Modifier.fillMaxWidth())

        Row(modifier = Modifier.padding(vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = {
                persist()
                onDone()
            }) { Text("Save") }
            OutlinedButton(onClick = onDone) { Text("Cancel") }
        }

        // ---- settings file export / import ----
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = {
                exportLauncher.launch("minilora-app-settings.json")
            }) { Text("Export settings") }
            OutlinedButton(onClick = {
                importLauncher.launch(arrayOf("*/*"))
            }) { Text("Import settings") }
        }
        if (fileMsg.isNotEmpty()) {
            Text(fileMsg, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Text("The settings file contains the TTN API key in clear text — " +
            "treat it like a credential. Device BLE fields travel separately " +
            "in the Config tab (.cfg).",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
