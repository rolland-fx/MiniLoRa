package co.minilora.app.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import co.minilora.app.ui.AppViewModel
import co.minilora.app.ui.Tab
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * BLE scan + device selection. Only devices whose advertisement carries a
 * valid MiniLoRa beacon frame are listed; tap to make a device the target
 * for config/status/TTN lookups.
 */
@Composable
fun DevicesScreen(vm: AppViewModel, modifier: Modifier = Modifier) {
    val hits by vm.scanHits.collectAsState()
    val scanning by vm.scanActive.collectAsState()
    val targetMac = co.minilora.app.util.DeviceConfig.targetMacAddress

    Column(modifier = modifier.fillMaxSize()) {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column {
                Text(if (scanning) "Scanning for beacons…" else "Beacon scanner",
                    style = MaterialTheme.typography.titleMedium)
                Text(
                    if (scanning) "${hits.size} MiniLoRa device(s) seen"
                    else "Tap the button to ${if (hits.isEmpty()) "start" else "rescan"}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (hits.isEmpty()) {
            Text(
                if (scanning) "Keep the tracker awake near the phone — its BLE beacon " +
                    "appears here automatically." else "No devices yet.",
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(16.dp),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        LazyColumn {
            items(hits, key = { it.device.address }) { hit ->
                val selected = hit.device.address.equals(targetMac, ignoreCase = true)
                Card(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp, vertical = 4.dp)
                        .clickable {
                            vm.selectDevice(hit.device.name, hit.device.address)
                        },
                ) {
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .padding(12.dp),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column {
                            Text(hit.device.name +
                                if (selected) "  ✓ selected" else "",
                                style = MaterialTheme.typography.bodyLarge)
                            Text("${hit.device.address} · seen ${TIME_FMT_SEEN.format(Date(hit.device.discoveredAt))}",
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        Text("${hit.device.rssi} dBm",
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.primary)
                    }
                }
            }
        }
    }
}

private val TIME_FMT_SEEN = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
