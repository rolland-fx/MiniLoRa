package co.minilora.app.ui.screens

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import co.minilora.app.ui.AppViewModel
import co.minilora.app.ui.model.PayloadHistoryEntry
import android.widget.Toast
import android.content.Context
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.view.ViewGroup
import androidx.annotation.DrawableRes
import androidx.core.content.ContextCompat
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DeleteSweep
import org.osmdroid.config.Configuration
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import androidx.compose.ui.draw.clipToBounds
import kotlin.math.abs
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val TIME_FMT = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
private val DATE_FMT = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault())

/**
 * Tracker home: live status header, the map (osmdroid in AndroidView) and
 * the payload history strip. The map shows the latest position as a red
 * marker and up to 9 older ones fading out.
 */
@Composable
fun TrackerScreen(vm: AppViewModel, modifier: Modifier = Modifier) {
    val history by vm.displayHistory.collectAsState()
    val deviceChips by vm.deviceChips.collectAsState()
    val selectedKey by vm.selectedDeviceKey.collectAsState()
    val status by vm.statusFrame.collectAsState()

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
        MiniMap(
            entries = history,
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
                HistoryCard(entry)
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
                    val p = latest.payload
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
 * Plain circular dots (latest = red 16dp, history = blue 14dp) — the
 * default osmdroid pin is too busy at tracker zoom levels.
 */
private fun dotIcon(context: Context, @DrawableRes res: Int, dp: Float):
        BitmapDrawable {
    val px = (dp * context.resources.displayMetrics.density).toInt()
    val bmp = android.graphics.Bitmap.createBitmap(px, px,
        android.graphics.Bitmap.Config.ARGB_8888)
    val d = ContextCompat.getDrawable(context, res)!!
    d.setBounds(0, 0, px, px)
    d.draw(Canvas(bmp))
    return BitmapDrawable(context.resources, bmp)
}

/** osmdroid map wrapped for Compose; markers rebuilt when entries change. */
@Composable
private fun MiniMap(entries: List<PayloadHistoryEntry>, modifier: Modifier = Modifier) {
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

    LaunchedEffect(entries) {
        val map = mapView
        map.overlays.clear()
        val valid = entries.filter {
            it.payload.latitude != 0.0 || it.payload.longitude != 0.0
        }.take(10)
        val latestDot = dotIcon(map.context,
            co.minilora.app.R.drawable.dot_marker, 16f)
        val historyDot = dotIcon(map.context,
            co.minilora.app.R.drawable.dot_marker_blue, 14f)
        valid.forEachIndexed { i, entry ->
            val marker = Marker(map).apply {
                position = GeoPoint(entry.payload.latitude, entry.payload.longitude)
                // dots are round: center on the coordinate, not bottom-tipped
                setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                icon = if (i == 0) latestDot else historyDot
                alpha = if (i == 0) 1.0f
                        else (1.0f - i.toFloat() / valid.size).coerceIn(0.2f, 0.8f)
                title = if (i == 0) "Latest" else "Position ${i + 1}"
                snippet = String.format(Locale.US, "%.5f, %.5f",
                    entry.payload.latitude, entry.payload.longitude)
            }
            map.overlays.add(marker)
        }
        valid.firstOrNull()?.let { first ->
            val gp = GeoPoint(first.payload.latitude, first.payload.longitude)
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
        map.invalidate()
    }
}

@Composable
private fun HistoryCard(entry: PayloadHistoryEntry) {
    Card(
        modifier = Modifier.padding(vertical = 4.dp),
        colors = CardDefaults.cardColors(),
    ) {
        Column(Modifier.padding(10.dp)) {
            val p = entry.payload
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
