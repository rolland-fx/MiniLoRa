package co.minilora.app.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import co.minilora.app.ui.AppViewModel
import co.minilora.app.ui.model.PayloadHistoryEntry
import co.minilora.app.util.DeviceConfig
import co.minilora.app.data.model.MiniLoRaCommands
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DeleteSweep
import android.widget.Toast
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.drawable.BitmapDrawable
import android.view.ViewGroup
import org.osmdroid.config.Configuration
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Polyline
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.draw.clipToBounds
import kotlin.math.abs
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val TIME_FMT = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
private val DATE_FMT = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault())

/**
 * Tracker home: live status header, trek peer contact cards, the map
 * (osmdroid in AndroidView) and the payload history strip. The map draws
 * one colored track per device (ICD §10.8: both trek positions visible
 * with different colors) — the latest point solid, history fading, all
 * joined by a polyline in the device's hue.
 */
@Composable
fun TrackerScreen(vm: AppViewModel, modifier: Modifier = Modifier) {
    val history by vm.displayHistory.collectAsState()
    val allHistory by vm.history.collectAsState()
    val deviceChips by vm.deviceChips.collectAsState()
    val selectedKey by vm.selectedDeviceKey.collectAsState()
    val status by vm.statusFrame.collectAsState()
    val peerContacts by vm.peerContacts.collectAsState()

    // Stable per-device colors from the FULL store (not the filter), so a
    // device keeps its hue whichever filter is active. The remembered
    // tracker always claims the first hue (blue), peers follow.
    val trackColors = remember(allHistory) {
        trackColorsFor(allHistory.map { it.deviceKey })
    }

    Column(modifier = modifier.fillMaxSize()) {
        var graphsOpen by remember { mutableStateOf(false) }
        if (deviceChips.size > 1) {
            Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState())
                       .padding(horizontal = 12.dp, vertical = 2.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                deviceChips.forEach { chip ->
                    FilterChip(
                        selected = chip.key == selectedKey,
                        onClick = { vm.selectDeviceFilter(chip.key) },
                        leadingIcon = if (chip.key.isBlank()) null else ({
                            Box(Modifier.size(10.dp).clip(CircleShape)
                                    .background(trackColors[chip.key]
                                        ?: TRACK_COLORS[0]))
                        }),
                        label = { Text(chip.label,
                            style = MaterialTheme.typography.labelSmall) },
                    )
                }
            }
        }
        StatusHeader(history.firstOrNull(), status,
            onOpenGraphs = if (history.isNotEmpty()) ({ graphsOpen = true })
                           else null)
        if (graphsOpen) {
            PayloadGraphsDialog(entries = history,
                onDismiss = { graphsOpen = false })
        }
        // Trek peer contact cards (ICD §10.8/§10.9): freshness + in-touch.
        peerContacts.values
            .sortedBy { it.originKey }
            .forEach { contact ->
                PeerContactCard(contact, trackColors[contact.originKey])
            }
        MiniMap(
            entries = history,
            trackColors = trackColors,
            nameFor = vm::displayNameOf,
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f)
                .padding(horizontal = 12.dp, vertical = 8.dp),
        )
        var pruneOpen by remember { mutableStateOf(false) }
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("History", style = MaterialTheme.typography.titleSmall)
            IconButton(
                onClick = { pruneOpen = true },
                modifier = Modifier.height(32.dp),
            ) {
                Icon(Icons.Filled.DeleteSweep,
                    contentDescription = "Clean data store",
                    modifier = Modifier.height(20.dp))
            }
        }
        if (pruneOpen) {
            val ctx = LocalContext.current
            PruneDialog(
                onApply = { opt ->
                    val deleted = vm.pruneHistory(opt)
                    Toast.makeText(
                        ctx,
                        if (deleted == 0) "Nothing to delete"
                        else "$deleted entries deleted",
                        Toast.LENGTH_SHORT,
                    ).show()
                },
                onDismiss = { pruneOpen = false },
            )
        }
        LazyRow(
            modifier = Modifier
                .fillMaxWidth()
                .height(120.dp)
                .padding(horizontal = 12.dp),
            reverseLayout = true,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(history, key = { it.hashCode() }) { entry ->
                HistoryCard(entry, vm.displayNameOf(entry.deviceKey))
            }
        }
    }
}

/** Human age for the hero card: "just now", "4 min ago", "3 h ago", "2 d ago". */
private fun ageLabel(timeUnix: Long): String {
    val ageS = System.currentTimeMillis() / 1000 - timeUnix
    return when {
        ageS < 90 -> "just now"
        ageS < 3600 -> "${ageS / 60} min ago"
        ageS < 86_400 -> "${ageS / 3600} h ago"
        else -> "${ageS / 86_400} d ago"
    }
}

@Composable
private fun StatusHeader(
    latest: PayloadHistoryEntry?,
    status: Map<String, Any>?,
    onOpenGraphs: (() -> Unit)? = null,
) {
    Card(
        onClick = { onOpenGraphs?.invoke() },
        enabled = latest != null,
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 8.dp),
    ) {
        Column(Modifier.padding(12.dp)) {
            if (latest == null) {
                Text("No data yet — scan for the beacon or enable TTN polling.",
                    style = MaterialTheme.typography.bodyMedium)
            } else {
                val p = latest.payload
                if (p == null) {
                    // Config-window hit: the tracker is advertising its
                    // config service, not a telemetry beacon.
                    Row(Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("Device in config mode",
                            style = MaterialTheme.typography.titleMedium)
                        AssistChip(onClick = {},
                            label = { Text(latest.source.label()) })
                    }
                } else {
                    val batV = p.batteryVoltage
                    val fix = p.latitude != 0.0 || p.longitude != 0.0
                    Row(Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("${p.temperatureCelsius}°C  ${p.humidityPercent}%  " +
                                String.format(Locale.US, "%.1f hPa", p.pressureHpa),
                            style = MaterialTheme.typography.titleMedium)
                        AssistChip(onClick = {},
                            label = { Text(latest.source.label()) })
                    }
                    Row(Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("Lux ${p.luminosityLux} · Acc " +
                                String.format(Locale.US, "%.2f/%.2f/%.2f g",
                                    p.accX / 1000.0, p.accY / 1000.0,
                                    p.accZ / 1000.0),
                            style = MaterialTheme.typography.bodySmall)
                        Text("$batV V · ${p.satellites} sats" +
                                if (fix) "" else " · no fix",
                            style = MaterialTheme.typography.bodySmall)
                    }
                    Row(Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween) {
                        Text(
                            if (fix) String.format(Locale.US, "%.5f, %.5f · %d m",
                                p.latitude, p.longitude, p.altitude)
                            else "GPS: no fix",
                            style = MaterialTheme.typography.bodySmall,
                        )
                        Text(
                            if (p.timeUnixSeconds > 0) DATE_FMT.format(p.date)
                            else "no GPS time",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    Row(Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("awake ${p.awakeSeconds} s · sleep ${p.sleepMinutes} min",
                            style = MaterialTheme.typography.bodySmall)
                        // When the device was heard (falls back to the
                        // receive wall-clock when the payload carries no
                        // GPS time).
                        Text(
                            "${ageLabel(latest.timeUnix)} · " +
                                TIME_FMT.format(Date(latest.timeUnix * 1000)),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            status?.let {
                val flags = mutableListOf<String>()
                if ((it["powerFlags"] as Int and 1) != 0) flags.add("LOW BAT")
                if ((it["powerFlags"] as Int and 2) != 0) flags.add("CRITICAL")
                if (it["recoveryMode"] == true) flags.add("RECOVERY")
                if (it["otaPending"] == true) flags.add("OTA PENDING")
                Text(
                    "Device: fw ${it["fwVersion"]} · bat ${it["batMv"]} mV · " +
                        "joined=${it["joined"]}" +
                        if (flags.isEmpty()) "" else "  [${flags.joinToString(",")}]",
                    style = MaterialTheme.typography.bodySmall,
                    color = if (flags.isEmpty()) MaterialTheme.colorScheme.onSurfaceVariant
                            else MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(top = 4.dp),
                )
            }
        }
    }
}

/**
 * Trek peer contact card (ICD §10.8): who the peer is, its latest relayed
 * telemetry and whether contact is fresh. "Lost" after 3 missed exchange
 * windows (3× the configured sleep period) without a relay — mirrors the
 * device LED signal.
 */
@Composable
private fun PeerContactCard(
    contact: AppViewModel.PeerContact,
    color: Color?,
) {
    // Re-evaluate freshness twice a minute so "contact lost" appears
    // without new data arriving.
    var tick by remember { mutableStateOf(0) }
    LaunchedEffect(Unit) {
        while (true) {
            kotlinx.coroutines.delay(30_000)
            tick++
        }
    }
    val periodMin = MiniLoRaCommands.SLEEP_MINUTES
        .getOrNull(DeviceConfig.deviceCfgRange) ?: 30
    val ageMs = System.currentTimeMillis() - contact.lastSeenMs
    val inTouch = ageMs < 3L * periodMin * 60_000
    val hue = color ?: TRACK_COLORS[1]
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 2.dp),
        colors = CardDefaults.cardColors(
            containerColor = if (inTouch) MaterialTheme.colorScheme.surface
                             else MaterialTheme.colorScheme.errorContainer),
    ) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(12.dp).clip(CircleShape).background(hue))
            Column(Modifier.weight(1f).padding(start = 8.dp)) {
                Text(
                    "Peer ${contact.originKey} · ${contact.payload.satellites} sats · " +
                        String.format(Locale.US, "%.1f V", contact.payload.batteryVoltage) +
                        " · ${contact.payload.temperatureCelsius}°C",
                    style = MaterialTheme.typography.bodySmall)
                Text(
                    "relayed via ${contact.relayMac} · seq ${contact.seq} · " +
                        ageLabel(contact.lastSeenMs / 1000),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Text(
                if (inTouch) "IN TOUCH" else "CONTACT LOST",
                style = MaterialTheme.typography.labelSmall,
                color = if (inTouch) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.error)
        }
    }
}

// ---- per-device track colors -------------------------------------------

/**
 * The trek contract: both positions visible with different colors. Stable
 * assignment — the remembered tracker claims the first hue, every other
 * identity follows alphabetically, so a device keeps its color across
 * recompositions.
 */
private val TRACK_COLORS = listOf(
    Color(0xFF1976D2),   // blue   — the classic tracker hue
    Color(0xFF388E3C),   // green
    Color(0xFF7B1FA2),   // purple
    Color(0xFFF57C00),   // orange
    Color(0xFF0097A7),   // cyan
)

internal fun trackColorsFor(deviceKeys: Collection<String>):
        Map<String, Color> {
    val target = DeviceConfig.targetMacAddress
    val ordered = deviceKeys.distinct().let { ks ->
        ks.filter { it.equals(target, ignoreCase = true) } +
            ks.filterNot { it.equals(target, ignoreCase = true) }.sorted()
    }
    return ordered.mapIndexed { i, k ->
        k to TRACK_COLORS[i % TRACK_COLORS.size]
    }.toMap()
}

/** Plain colored circular dot — the default osmdroid pin is too busy at
 *  tracker zoom levels. Drawn in-code (no drawable juggling per hue). */
private fun dotIcon(context: Context, color: Color, dp: Float): BitmapDrawable {
    val px = (dp * context.resources.displayMetrics.density).toInt()
    val bmp = Bitmap.createBitmap(px, px, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(bmp)
    val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = color.toArgb()
        style = Paint.Style.FILL
    }
    canvas.drawCircle(px / 2f, px / 2f, px / 2f - 1f, paint)
    val border = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = android.graphics.Color.WHITE
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * context.resources.displayMetrics.density
    }
    canvas.drawCircle(px / 2f, px / 2f, px / 2f - 1f, border)
    return BitmapDrawable(context.resources, bmp)
}

/** osmdroid map wrapped for Compose; one colored track per device. */
@Composable
private fun MiniMap(entries: List<PayloadHistoryEntry>,
                    trackColors: Map<String, Color>,
                    nameFor: (String) -> String,
                    modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val mapView = remember {
        Configuration.getInstance().load(
            context, context.getSharedPreferences("osmdroid", 0))
        Configuration.getInstance().userAgentValue = "co.minilora.app"
        MapView(context).apply {
            setTileSource(org.osmdroid.tileprovider.tilesource.TileSourceFactory.MAPNIK)
            setMultiTouchControls(true)
            controller.setZoom(14.0)
            // Pin the view to exactly the Compose-given bounds: osmdroid
            // calls requestLayout() during zoom gestures, and with default
            // layout params the view could resize and push the header /
            // history strip around (bench 2026-11-01).
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT)
        }
    }
    // The user owns the zoom once the first fix lands: recenter only when
    // the reported position actually moves, and set the initial zoom once —
    // the old code reset zoom to 14 on EVERY history emission, snapping it
    // back while the user was pinch-zooming.
    var lastCenter by remember { mutableStateOf<GeoPoint?>(null) }
    var zoomSet by remember { mutableStateOf(false) }
    DisposableEffect(Unit) {
        mapView.onResume()
        onDispose { mapView.onPause() }
    }
    AndroidView(
        factory = { mapView },
        // Overflow is clipped — the map can never cover the header/history.
        modifier = modifier.clipToBounds(),
    )

    LaunchedEffect(entries, trackColors) {
        val map = mapView
        map.overlays.clear()
        // One track per device, oldest → newest so the polyline reads as
        // movement; latest dot solid, history dots fading in the same hue.
        val byDevice = entries.filter {
            it.payload.latitude != 0.0 || it.payload.longitude != 0.0
        }.groupBy { it.deviceKey }
        for ((deviceKey, deviceEntries) in byDevice) {
            val hue = trackColors[deviceKey] ?: TRACK_COLORS[0]
            val ordered = deviceEntries.sortedBy { it.timeUnix }
            val points = ordered.map {
                GeoPoint(it.payload.latitude, it.payload.longitude)
            }
            if (points.size >= 2) {
                val line = Polyline(map).apply {
                    setPoints(points)
                    outlinePaint.color = hue.toArgb()
                    outlinePaint.strokeWidth = 4f * map.context.resources.displayMetrics.density
                    outlinePaint.alpha = 140
                }
                map.overlays.add(line)
            }
            val latestDot = dotIcon(map.context, hue, 16f)
            val historyDot = dotIcon(map.context, hue, 12f)
            val shown = ordered.takeLast(10)
            shown.forEachIndexed { i, entry ->
                val isLatest = i == shown.size - 1
                val marker = Marker(map).apply {
                    position = GeoPoint(entry.payload.latitude,
                        entry.payload.longitude)
                    // dots are round: center on the coordinate, not bottom-tipped
                    setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                    icon = if (isLatest) latestDot else historyDot
                    alpha = if (isLatest) 1.0f
                            else (1.0f - (shown.size - 1 - i) / 10f)
                                .coerceIn(0.25f, 0.8f)
                    title = (if (isLatest) "Latest · " else "") + nameFor(deviceKey)
                    snippet = String.format(Locale.US, "%.5f, %.5f",
                        entry.payload.latitude, entry.payload.longitude)
                }
                map.overlays.add(marker)
            }
        }
        entries.firstOrNull()?.payload?.let { p ->
            if (p.latitude != 0.0 || p.longitude != 0.0) {
                val gp = GeoPoint(p.latitude, p.longitude)
                val prev = lastCenter
                val moved = prev == null ||
                    abs(gp.latitude - prev.latitude) > 1e-6 ||
                    abs(gp.longitude - prev.longitude) > 1e-6
                if (moved) {
                    map.controller.setCenter(gp)
                    if (!zoomSet) {
                        map.controller.setZoom(14.0)
                        zoomSet = true
                    }
                    lastCenter = gp
                }
            }
        }
        map.invalidate()
    }
}

@Composable
private fun HistoryCard(entry: PayloadHistoryEntry, deviceName: String) {
    Card(
        modifier = Modifier.padding(vertical = 4.dp),
        colors = CardDefaults.cardColors(),
    ) {
        Column(Modifier.padding(10.dp)) {
            val p = entry.payload
            Text(deviceName, style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.primary)
            Text("${p.temperatureCelsius}°C · ${p.humidityPercent}% · " +
                    String.format(Locale.US, "%.1f hPa", p.pressureHpa),
                style = MaterialTheme.typography.bodySmall)
            Text(
                TIME_FMT.format(Date(entry.timeUnix * 1000)) + " · " + entry.source.label(),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            val fix = p.latitude != 0.0 || p.longitude != 0.0
            Text(
                if (fix) String.format(Locale.US, "%.5f, %.5f", p.latitude, p.longitude)
                else "no fix",
                style = MaterialTheme.typography.labelSmall,
                color = if (fix) MaterialTheme.colorScheme.tertiary
                        else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}
