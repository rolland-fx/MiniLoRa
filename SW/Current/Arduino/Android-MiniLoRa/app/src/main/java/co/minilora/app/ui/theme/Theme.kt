package co.minilora.app.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

// Tracker palette: deep teal primary (radio/field vibe), amber accents for
// battery states, green for "healthy".
private val Teal = Color(0xFF00897B)
private val TealDark = Color(0xFF004D40)
private val Amber = Color(0xFFFFB300)
private val Green = Color(0xFF43A047)
private val Red = Color(0xFFE53935)

private val LightScheme = lightColorScheme(
    primary = Teal,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFB2DFDB),
    secondary = Amber,
    tertiary = Green,
    error = Red,
)

private val DarkScheme = darkColorScheme(
    primary = Color(0xFF80CBC4),
    onPrimary = Color(0xFF00332D),
    primaryContainer = TealDark,
    secondary = Amber,
    tertiary = Green,
    error = Red,
)

@Composable
fun MiniLoRaTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val context = LocalContext.current
    val scheme = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> DarkScheme
        else -> LightScheme
    }
    MaterialTheme(colorScheme = scheme, content = content)
}

// Shared status colors (used across screens)
object StatusColors {
    @Composable fun ok() = MaterialTheme.colorScheme.tertiary
    @Composable fun warn() = Color(0xFFFFB300)
    @Composable fun bad() = MaterialTheme.colorScheme.error
}
