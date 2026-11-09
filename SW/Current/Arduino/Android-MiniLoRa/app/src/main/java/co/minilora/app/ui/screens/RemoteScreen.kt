package co.minilora.app.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import co.minilora.app.data.model.MiniLoRaCommands
import co.minilora.app.data.model.MiniLoRaDiag
import co.minilora.app.ui.AppViewModel
import co.minilora.app.ui.SectionCard
import co.minilora.app.ui.model.PayloadHistoryEntry
import co.minilora.app.util.DeviceConfig
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** When a downlink command sent now will actually take effect.
 *
 * Delivery rides the RX window of the device's NEXT uplink (class A),
 * and the command is applied at the start of the cycle after that
 * (ICD §3) — i.e. two wake cycles from the last uplink. Cycle length =
 * the payload's sleep minutes + ~1 min of awake overhead (observed
 * 15.2-15.7 min for sleep 15); degraded battery doubles the interval.
 */
private data class CommandTiming(
    val etaSec: Long,        // when the command takes effect
    val lastUplinkSec: Long, // the latest receive data time it is based on
    val cycleSec: Int,
)

private fun commandTiming(entries: List<PayloadHistoryEntry>,
                          degraded: Boolean, nowSec: Long): CommandTiming? {
    val latest = entries.maxByOrNull { it.timeUnix } ?: return null
    val sleepMin = latest.payload.sleepMinutes.takeIf { it in 1..120 } ?: 15
    var cycleSec = (sleepMin + 1) * 60
    if (degraded) cycleSec *= 2
    // first wake at or after now = delivery uplink (index k); the command
    // applies in wake k+1
    val k = ((nowSec - latest.timeUnix).coerceAtLeast(0L) / cycleSec).toInt() + 1
    return CommandTiming(latest.timeUnix + (k + 1) * cycleSec,
                         latest.timeUnix, cycleSec)
}

/**
 * Remote tab: the three *device* concerns — commands (ICD §3), the
 * diagnostics dashboard, and remote config (ICD 3.5/3.6). Polling toggles
 * live in Settings; this screen is only what reaches the device.
 */
@Composable
fun RemoteScreen(vm: AppViewModel, modifier: Modifier = Modifier) {
    val diag by vm.diag.collectAsState()
    val diagReceivedAt by vm.diagReceivedAt.collectAsState()
    val history by vm.history.collectAsState()
    val scope = androidx.compose.runtime.rememberCoroutineScope()
    // (timing, wasBleWindow) of the last command sent — drives the ETA line
    var lastCmd by remember { mutableStateOf<Pair<CommandTiming, Boolean>?>(null) }

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        // ---- commands ----
        SectionCard("Commands",
            "Queued to the device's TTN downlink; applied at the start of its next wake cycle.") {
            Column(Modifier.fillMaxWidth(),
                   verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = {
                        vm.sendCommand(MiniLoRaCommands.LOCATE_NOW, "Locate now")
                        lastCmd = commandTiming(history, diag?.degraded ?: false,
                            System.currentTimeMillis() / 1000)?.let { it to false }
                    }, modifier = Modifier.weight(1f)) { Text("Locate now") }
                    Button(onClick = {
                        vm.sendCommand(MiniLoRaCommands.SEND_DIAG, "Diagnostics")
                        lastCmd = commandTiming(history, diag?.degraded ?: false,
                            System.currentTimeMillis() / 1000)?.let { it to false }
                    }, modifier = Modifier.weight(1f)) { Text("Send diag") }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = {
                        vm.sendCommand(MiniLoRaCommands.RESET_DIAG, "Counter reset")
                        lastCmd = commandTiming(history, diag?.degraded ?: false,
                            System.currentTimeMillis() / 1000)?.let { it to false }
                    }, modifier = Modifier.weight(1f)) { Text("Reset counters") }
                    OutlinedButton(onClick = {
                        vm.sendCommand(MiniLoRaCommands.BLE_WINDOW, "BLE window")
                        lastCmd = commandTiming(history, diag?.degraded ?: false,
                            System.currentTimeMillis() / 1000)?.let { it to true }
                    }, modifier = Modifier.weight(1f)) { Text("Open BLE window") }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = {
                        vm.sendCommand(MiniLoRaCommands.OTA_UPDATE, "Firmware update")
                        lastCmd = commandTiming(history, diag?.degraded ?: false,
                            System.currentTimeMillis() / 1000)?.let { it to false }
                    }, modifier = Modifier.weight(1f)) { Text("Update firmware") }
                }
                lastCmd?.let { (t, isBle) ->
                    val fmt = remember { SimpleDateFormat("HH:mm", Locale.getDefault()) }
                    val etaStr = fmt.format(Date(t.etaSec * 1000))
                    Text(
                        if (isBle) "BLE window opens ≈ $etaStr — keep the app " +
                            "scanning from then; the device advertises its " +
                            "config service for 120 s."
                        else "Takes effect ≈ $etaStr (based on the " +
                            "${fmt.format(Date(t.lastUplinkSec * 1000))} uplink, " +
                            "≈ ${t.cycleSec / 60} min cycles; a missed downlink " +
                            "slips one cycle).",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        // ---- downlink queue ----
        val downlinks by vm.downlinks.collectAsState()
        // While a command is pending, refresh quietly every 30 s: the
        // pending→sent flip and the diag confirm both come from pulls.
        val anyPending = downlinks.any { it.status == "pending" }
        LaunchedEffect(anyPending) {
            while (anyPending) {
                delay(30_000)
                if (DeviceConfig.ttnConfigured()) vm.fetchTTNNow()
                else if (DeviceConfig.wifiConfigured()) vm.fetchWiFiNow()
            }
        }
        // Receipts (sent/replaced) expire after 30 min — purge hourly-ish.
        LaunchedEffect(Unit) {
            while (true) {
                delay(5 * 60_000L)
                vm.purgeExpiredDownlinks()
            }
        }
        if (downlinks.isNotEmpty()) {
            SectionCard("Downlink queue", null) {
                Column(Modifier.fillMaxWidth(),
                       verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    val fmt = remember {
                        SimpleDateFormat("HH:mm:ss", Locale.getDefault())
                    }
                    downlinks.asReversed().forEach { e ->
                        Row(Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween) {
                            Text("${e.label} · ${e.hex}",
                                style = MaterialTheme.typography.bodySmall)
                            Text(
                                // "sent" stamps the delivery (the confirming
                                // uplink's time), not the queue time — the
                                // same rule as the diag card's send stamp.
                                "${e.status} · " + fmt.format(Date(
                                    if (e.sentAtMs > 0) e.sentAtMs
                                    else e.queuedAtMs)),
                                style = MaterialTheme.typography.labelSmall,
                                color = when (e.status) {
                                    "pending" -> MaterialTheme.colorScheme.primary
                                    "sent" -> MaterialTheme.colorScheme.onSurfaceVariant
                                    else -> MaterialTheme.colorScheme.error
                                },
                            )
                        }
                    }
                    Text(
                        "One command rides each wake cycle's RX window; a " +
                            "newer command replaces a still-pending one. " +
                            "Completed commands clear after 30 min.",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (anyPending) {
                        OutlinedButton(
                            onClick = { vm.clearDownlinkQueue() },
                            modifier = Modifier.fillMaxWidth(),
                        ) { Text("Clear TTN downlink queue") }
                    }
                }
            }
        }

        // ---- diagnostics ----
        if (diag == null) {
            Text("No diagnostics frame received yet (device sends one every ~16 boots, or on demand).",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            val d = diag!!
            SectionCard("Diagnostics", null) {
                Column(Modifier.fillMaxWidth()) {
                    Text("fw ${d.fwVersion} · boot #${d.bootCount}",
                        style = MaterialTheme.typography.titleSmall)
                    diagReceivedAt?.let { at ->
                        val fmt = remember {
                            SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())
                        }
                        Text(
                            "Sent by device ${fmt.format(Date(at))}",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    val flags = buildList {
                        if (d.degraded) add("LOW BATTERY")
                        if (d.critical) add("CRITICAL")
                        if (d.recoveryMode) add("RECOVERY")
                        if (d.otaPending) add("OTA PENDING")
                        if (d.gpsValid) add("GPS OK")
                    }
                    if (flags.isNotEmpty()) {
                        Text(flags.joinToString("  "),
                            style = MaterialTheme.typography.labelMedium,
                            color = if (d.critical || d.recoveryMode)
                                MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.primary)
                    }
                    CounterRow("GPS timeouts", d.gpsTimeouts)
                    CounterRow("LoRa join failures", d.loraJoinFailures)
                    CounterRow("LoRa send failures", d.loraSendFailures)
                    CounterRow("WiFi failures", d.wifiFailures)
                    CounterRow("Zigbee failures", d.zigbeeFailures)
                    CounterRow("Sensor read failures", d.sensorReadFailures)
                    CounterRow("Unexpected resets", d.unexpectedResets)
                    CounterRow("Consecutive GPS fails", d.gpsConsecFails)
                    d.lastTtff?.let { CounterRow("Last TTFF", it) }
                    d.linkMargin?.let { CounterRow("Link margin", it) }
                    d.gwCnt?.let { CounterRow("Gateways", it) }
                    d.otaStatus?.let { CounterRow("Last OTA status", it) }
                    OutlinedButton(
                        onClick = { scope.launch { vm.fetchTTNNow() } },
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("Fetch now (TTN + history)") }
                }
            }
        }

        // ---- remote config (ICD 3.5 §3 CFG_SET) ----
        SectionCard("Remote config",
            "Toggles a transport or sensor on the device (saved in NVS, " +
                "survives resets). The switch state comes from the latest " +
                "diag frame and confirms one cycle after queuing. LoRaWAN " +
                "is always on; BLE config (long-press) always works.") {
            Column(Modifier.fillMaxWidth(),
                   verticalArrangement = Arrangement.spacedBy(4.dp)) {
                // (label, CFG_SET flag id, diag configFlags bit)
                val rows = listOf(
                    Triple("WiFi", MiniLoRaCommands.CFG_WIFI,
                           MiniLoRaDiag.CFG_WIFI),
                    Triple("Zigbee", MiniLoRaCommands.CFG_ZIGBEE,
                           MiniLoRaDiag.CFG_ZIGBEE),
                    Triple("BLE beacon", MiniLoRaCommands.CFG_BLE,
                           MiniLoRaDiag.CFG_BLE),
                    Triple("GNSS", MiniLoRaCommands.CFG_GNSS,
                           MiniLoRaDiag.CFG_GNSS),
                    Triple("BME280", MiniLoRaCommands.CFG_BME280,
                           MiniLoRaDiag.CFG_BME280),
                    Triple("OPT3001", MiniLoRaCommands.CFG_OPT3001,
                           MiniLoRaDiag.CFG_OPT3001),
                    Triple("LIS3DH", MiniLoRaCommands.CFG_LIS3DH,
                           MiniLoRaDiag.CFG_LIS3DH),
                )
                rows.forEach { (name, flagId, mask) ->
                    // null = no schema-0x05 diag yet (state unknown)
                    val state = diag?.cfgFlag(mask)
                    Row(Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically) {
                        Column {
                            Text(name, style = MaterialTheme.typography.bodyLarge)
                            Text(
                                when (state) {
                                    true -> "on"; false -> "off"
                                    null -> "unknown until next diag"
                                },
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                            OutlinedButton(
                                onClick = { vm.sendConfigSet(flagId, true,
                                                             "$name ON") },
                                enabled = state != true,
                            ) { Text("ON") }
                            OutlinedButton(
                                onClick = { vm.sendConfigSet(flagId, false,
                                                             "$name OFF") },
                                enabled = state != false,
                            ) { Text("OFF") }
                        }
                    }
                }

                // ---- sleep duration (ICD 3.6 §3 SET_RANGE) ----
                // Current value from the newest history payload's
                // sleep-minutes byte; the confirm rides the next frame.
                var sleepMenu by remember { mutableStateOf(false) }
                val curSleepMin = history
                    .maxByOrNull { it.timeUnix }?.payload?.sleepMinutes
                val curIdx = curSleepMin?.let {
                    MiniLoRaCommands.SLEEP_LABELS.indexOf("$it min")
                } ?: -1
                Row(Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    Column {
                        Text("Sleep duration",
                             style = MaterialTheme.typography.bodyLarge)
                        Text(
                            if (curIdx >= 0) "current: ${MiniLoRaCommands.SLEEP_LABELS[curIdx]}"
                            else "unknown until next reading",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Box {
                        OutlinedButton(onClick = { sleepMenu = true }) {
                            Text(if (curIdx >= 0)
                                     MiniLoRaCommands.SLEEP_LABELS[curIdx]
                                 else "set…")
                        }
                        DropdownMenu(expanded = sleepMenu,
                                     onDismissRequest = { sleepMenu = false }) {
                            MiniLoRaCommands.SLEEP_LABELS.forEachIndexed { i, label ->
                                DropdownMenuItem(
                                    text = { Text(label) },
                                    onClick = {
                                        sleepMenu = false
                                        vm.sendCommand(
                                            MiniLoRaCommands.SET_RANGE,
                                            "sleep $label", i)
                                    },
                                    enabled = i != curIdx,
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun CounterRow(label: String, value: Int) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, style = MaterialTheme.typography.bodySmall)
        Text("$value",
            style = MaterialTheme.typography.bodySmall,
            color = if (value > 0) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
