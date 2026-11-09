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
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import co.minilora.app.ui.AppViewModel
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * BLE scan + device selection. Every device whose advertisement carries a
 * valid MiniLoRa frame — its own beacon, a trek peer relay (§10.9) or the
 * config window — is listed; tap to make it the target for provisioning
 * and status reads.
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
                Text(if (scanning) "Scanning…" else "Nearby MiniLoRa devices",
                    style = MaterialTheme.typography.titleMedium)
                Text(
                    if (scanning)
                        "${hits.size} device(s) — tap one to select it for Setup"
                    else "Press the scan button (bottom right) to start",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (hits.isEmpty()) {
            Text(
                if (scanning)
                    "Keep the tracker awake near the phone — its BLE beacon " +
                        "appears here automatically. To provision, wake the " +
                        "device into its config window (double-press the button)."
                else "No devices yet.",
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
                    colors = if (selected) CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.primaryContainer)
                    else CardDefaults.cardColors(),
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
                            Text(
                                "${hit.device.address} · seen " +
                                    TIME_FMT_SEEN.format(Date(hit.device.discoveredAt)),
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                            // Trek: this tracker relays its peer's position
                            hit.peer?.let { peer ->
                                Text(
                                    "relaying peer ${peer.deviceKey} (seq ${peer.seq})",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.tertiary)
                            }
                        }
                        val (rssiColor, rssiLabel) = rssiQuality(hit.device.rssi)
                        Column(horizontalAlignment = Alignment.End) {
                            Text("${hit.device.rssi} dBm",
                                style = MaterialTheme.typography.labelLarge,
                                color = rssiColor)
                            Text(rssiLabel,
                                style = MaterialTheme.typography.labelSmall,
                                color = rssiColor)
                        }
                    }
                }
            }
        }
    }
}

/** RSSI → (color, quality word) — a glanceable link estimate. */
private fun rssiQuality(rssi: Int): Pair<Color, String> = when {
    rssi >= -70 -> Color(0xFF388E3C) to "excellent"
    rssi >= -85 -> Color(0xFFF57C00) to "good"
    else -> Color(0xFFD32F2F) to "weak"
}

private val TIME_FMT_SEEN = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
