package co.minilora.app.ui.screens

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import co.minilora.app.ui.model.PayloadHistoryEntry
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs

/**
 * Popup over the Tracker tab's latest-data card: every payload field except
 * position (that lives on the map) graphed over the app's history — the
 * Compose counterpart of the server's device-page charts. Charts are plain
 * Canvas line drawings, mirroring the server palette; no chart library.
 *
 * Sentinels are dropped like the server's analytics does: humidity 255,
 * lux 0xFFFF, satellites 255, implausible pressure (which also excludes
 * the pre-ICD-3.3 rows read 10× low).
 */

private data class Line(val color: Color, val pts: List<Pair<Long, Float>>)

private data class Series(
    val title: String,
    val unit: String,
    val lines: List<Line>,
    val legend: List<Pair<String, Color>> = emptyList(),
)

private val C_TEMP = Color(0xFFE65100)
private val C_HUM = Color(0xFF0277BD)
private val C_PRES = Color(0xFF6A1B9A)
private val C_BAT = Color(0xFF2E7D32)
private val C_LUX = Color(0xFFF9A825)
private val C_AX = Color(0xFFD32F2F)
private val C_AY = Color(0xFF388E3C)
private val C_AZ = Color(0xFF1976D2)
private val C_SAT = Color(0xFF546E7A)
private val C_ALT = Color(0xFF795548)

private val X_FMT = SimpleDateFormat("HH:mm", Locale.getDefault())

/** Per-chart point cap — drawing stays cheap even with a long history. */
private const val MAX_POINTS = 200

private fun fmt(v: Float): String = when {
    abs(v) >= 100f -> String.format(Locale.US, "%.0f", v)
    abs(v) >= 10f -> String.format(Locale.US, "%.1f", v)
    else -> String.format(Locale.US, "%.2f", v)
}

private fun buildSeries(entries: List<PayloadHistoryEntry>): List<Series> {
    val e = entries.sortedBy { it.timeUnix }.takeLast(MAX_POINTS)
    fun pts(keep: (Float) -> Boolean = { true }, f: (PayloadHistoryEntry) -> Float) =
        e.mapNotNull { en -> val v = f(en); if (keep(v)) en.timeUnix to v else null }

    return listOf(
        Series("Temperature", "°C",
            listOf(Line(C_TEMP, pts { it.payload.temperatureCelsius }))),
        Series("Humidity", "%",
            listOf(Line(C_HUM, pts({ it != 255f }) { it.payload.humidityPercent.toFloat() }))),
        Series("Pressure", "hPa",
            listOf(Line(C_PRES, pts({ it in 300f..1200f }) { it.payload.pressureHpa }))),
        Series("Battery", "V",
            listOf(Line(C_BAT, pts({ it in 2f..5f }) { it.payload.batteryVoltage }))),
        Series("Light", "lx",
            listOf(Line(C_LUX, pts({ it != 65535f }) { it.payload.luminosityLux.toFloat() }))),
        Series("Acceleration", "g",
            listOf(
                Line(C_AX, pts { (it.payload.accX / 1000.0).toFloat() }),
                Line(C_AY, pts { (it.payload.accY / 1000.0).toFloat() }),
                Line(C_AZ, pts { (it.payload.accZ / 1000.0).toFloat() }),
            ),
            legend = listOf("X" to C_AX, "Y" to C_AY, "Z" to C_AZ)),
        Series("Satellites", "",
            listOf(Line(C_SAT, pts({ it != 255f }) { it.payload.satellites.toFloat() }))),
        Series("Altitude", "m",
            listOf(Line(C_ALT, pts { it.payload.altitude.toFloat() }))),
    )
}

/** Selectable graph window: hours back from the newest point, null = the
 *  whole persisted timeline. 48 h of 15-min cycles is 192 points — under
 *  the 200-point draw cap — so every fixed range renders unclipped. */
private val RANGES: List<Pair<Int?, String>> = listOf(
    1 to "1 h", 3 to "3 h", 6 to "6 h", 12 to "12 h",
    24 to "24 h", 48 to "48 h", null to "All")

@Composable
fun PayloadGraphsDialog(entries: List<PayloadHistoryEntry>,
                        onDismiss: () -> Unit) {
    var rangeH by remember { mutableStateOf<Int?>(null) }
    // Evaluated against NOW at (re)build time: each poll slides the window
    // forward, picking a chip or reopening the dialog re-anchors it.
    val window = remember(entries, rangeH) {
        val r = rangeH
        if (r == null) entries
        else {
            val cutoff = System.currentTimeMillis() / 1000 - r * 3600L
            entries.filter { it.timeUnix >= cutoff }
        }
    }
    val series = remember(window) { buildSeries(window) }
    val rangeLabel = RANGES.firstOrNull { it.first == rangeH }?.second ?: "All"
    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = MaterialTheme.colorScheme.surface,
            modifier = Modifier
                .fillMaxWidth()
                .fillMaxHeight(0.88f)
                .padding(16.dp),
        ) {
            Column(Modifier.padding(vertical = 8.dp)) {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column {
                        Text("Sensor history",
                            style = MaterialTheme.typography.titleMedium)
                        // buildSeries plots the newest MAX_POINTS only — say
                        // so instead of a header count that outruns the ink.
                        Text(
                            (if (window.size > MAX_POINTS)
                                "last $MAX_POINTS of ${window.size}"
                             else "${window.size}") +
                                " points · $rangeLabel",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    IconButton(onClick = onDismiss) {
                        Icon(Icons.Filled.Close, contentDescription = "Close")
                    }
                }
                Row(
                    Modifier.fillMaxWidth()
                        .horizontalScroll(rememberScrollState())
                        .padding(horizontal = 16.dp, vertical = 2.dp),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    RANGES.forEach { (h, label) ->
                        FilterChip(
                            selected = rangeH == h,
                            onClick = { rangeH = h },
                            label = { Text(label,
                                style = MaterialTheme.typography.labelSmall) },
                        )
                    }
                }
                LazyColumn {
                    items(series) { s -> ChartPanel(s) }
                }
            }
        }
    }
}

@Composable
private fun ChartPanel(series: Series) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) {
        Row(Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween) {
            Text(series.title, style = MaterialTheme.typography.titleSmall)
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (series.legend.isNotEmpty()) {
                    Text(
                        buildAnnotatedString {
                            series.legend.forEachIndexed { i, (name, color) ->
                                if (i > 0) append("  ")
                                withStyle(SpanStyle(color = color)) { append("●") }
                                append(name)
                            }
                        },
                        style = MaterialTheme.typography.labelSmall,
                    )
                }
                if (series.unit.isNotEmpty()) {
                    if (series.legend.isNotEmpty()) Spacer(Modifier.width(8.dp))
                    Text(series.unit,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        Surface(
            color = MaterialTheme.colorScheme.surfaceVariant,
            shape = RoundedCornerShape(8.dp),
            modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
        ) {
            LineChart(series, Modifier.fillMaxWidth().height(130.dp).padding(4.dp))
        }
    }
}

@Composable
private fun LineChart(series: Series, modifier: Modifier = Modifier) {
    val textMeasurer = rememberTextMeasurer()
    val labelColor = MaterialTheme.colorScheme.onSurfaceVariant
    val gridColor = labelColor.copy(alpha = 0.25f)
    val labelStyle = TextStyle(fontSize = 9.sp, color = labelColor)
    Canvas(modifier) {
        val all = series.lines.flatMap { it.pts }
        if (all.size < 2) {
            val msg = if (all.isEmpty()) "no data" else "not enough data"
            val layout = textMeasurer.measure(msg, labelStyle)
            drawText(layout,
                topLeft = Offset(
                    (size.width - layout.size.width) / 2f,
                    (size.height - layout.size.height) / 2f))
            return@Canvas
        }
        val t0 = all.minOf { it.first }.toFloat()
        val t1 = all.maxOf { it.first }.toFloat()
        val tSpan = (t1 - t0).takeIf { it > 0f } ?: 1f
        var vmin = all.minOf { it.second }
        var vmax = all.maxOf { it.second }
        if (vmin == vmax) { vmin -= 1f; vmax += 1f }
        val vSpan = vmax - vmin

        val padT = 14.dp.toPx()   // room for the max label
        val padB = 14.dp.toPx()   // room for the min + time labels
        val h = size.height - padT - padB

        fun xy(t: Float, v: Float) = Offset(
            (t - t0) / tSpan * size.width,
            padT + (1f - (v - vmin) / vSpan) * h)

        // horizontal grid: top, middle, bottom
        listOf(0f, 0.5f, 1f).forEach { f ->
            val y = padT + f * h
            drawLine(gridColor, Offset(0f, y), Offset(size.width, y),
                strokeWidth = 1f)
        }

        series.lines.forEach { line ->
            if (line.pts.isEmpty()) return@forEach
            if (line.pts.size == 1) {
                drawCircle(line.color, radius = 3.dp.toPx(),
                    center = xy(line.pts[0].first.toFloat(),
                        line.pts[0].second))
            } else {
                val path = Path()
                line.pts.forEachIndexed { i, (t, v) ->
                    val p = xy(t.toFloat(), v)
                    if (i == 0) path.moveTo(p.x, p.y) else path.lineTo(p.x, p.y)
                }
                drawPath(path, line.color,
                    style = Stroke(width = 2.dp.toPx()))
            }
        }

        // labels: y max / y min (left), time range (top/bottom right).
        // A multi-day span needs the date in the labels — HH:mm alone
        // would read identically on every day of the gap it is naming.
        val xFmt = if (tSpan > 86_400f)
            SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()) else X_FMT
        drawText(textMeasurer, fmt(vmax), topLeft = Offset(2.dp.toPx(), 0f),
            style = labelStyle)
        drawText(textMeasurer, fmt(vmin), style = labelStyle,
            topLeft = Offset(2.dp.toPx(), size.height - 12.dp.toPx()))
        drawText(textMeasurer, xFmt.format(Date(t0.toLong() * 1000)),
            style = labelStyle,
            topLeft = Offset(size.width - 58.dp.toPx(),
                size.height - 12.dp.toPx()))
        drawText(textMeasurer, xFmt.format(Date(t1.toLong() * 1000)),
            style = labelStyle,
            topLeft = Offset(size.width - 58.dp.toPx(), 0f))
    }
}
