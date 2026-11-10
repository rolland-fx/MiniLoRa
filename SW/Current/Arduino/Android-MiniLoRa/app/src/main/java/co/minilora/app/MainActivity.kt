package co.minilora.app

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Context
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import co.minilora.app.ui.AppViewModel
import co.minilora.app.ui.MiniLoRaApp
import co.minilora.app.ui.screens.HelpScreen
import co.minilora.app.ui.screens.SettingsScreen
import co.minilora.app.ui.theme.MiniLoRaTheme
import co.minilora.app.util.DeviceConfig
import kotlinx.coroutines.launch
import org.osmdroid.config.Configuration

/**
 * Compose host: theme + one ViewModel + tab navigation. All behaviour lives
 * in [AppViewModel]; the screens in ui/screens are pure rendering.
 */
class MainActivity : ComponentActivity() {

    private val vm: AppViewModel by viewModels()
    private var showSettings by mutableStateOf(false)
    private var showHelp by mutableStateOf(false)

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { perms ->
        if (perms.values.any { !it }) {
            Toast.makeText(this, "Missing permissions: " +
                perms.filterValues { !it }.keys.joinToString(", "),
                Toast.LENGTH_LONG).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        DeviceConfig.init(this)
        Configuration.getInstance().load(
            this, getSharedPreferences("osmdroid", 0))
        Configuration.getInstance().userAgentValue = "co.minilora.app"
        requestBlePermissions()
        if (bleAdapter()?.isEnabled == false) {
            startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        }

        setContent {
            MiniLoRaTheme {
                when {
                    showHelp -> HelpScreen(onDone = { showHelp = false })
                    showSettings ->
                        SettingsScreen(vm, onDone = { showSettings = false })
                    else -> MiniLoRaApp(vm, onOpenSettings = { showSettings = true },
                                        onOpenHelp = { showHelp = true })
                }
            }
        }

        // Refresh diag at startup if TTN is configured (diag is on Remote tab)
        if (DeviceConfig.ttnConfigured()) {
            lifecycleScope.launch { vm.fetchTTNNow() }
        }
    }

    fun bleAdapter() = vm.bleAdapter

    override fun onResume() {
        super.onResume()
        vm.onAppResume()
    }

    override fun onPause() {
        super.onPause()
        vm.stopScanOnPause()
    }

    private fun requestBlePermissions() {
        val needed = mutableListOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.ACCESS_FINE_LOCATION,
        ).filter {
            ContextCompat.checkSelfPermission(this, it) !=
                PackageManager.PERMISSION_GRANTED
        }
        if (needed.isNotEmpty()) {
            permissionLauncher.launch(needed.toTypedArray())
        }
    }
}
