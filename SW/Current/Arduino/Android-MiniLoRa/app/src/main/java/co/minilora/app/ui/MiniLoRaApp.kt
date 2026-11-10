package co.minilora.app.ui

import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BluetoothDisabled
import androidx.compose.material.icons.filled.BluetoothSearching
import androidx.compose.material.icons.filled.HelpOutline
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SettingsRemote
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import co.minilora.app.ui.screens.ConfigScreen
import co.minilora.app.ui.screens.DevicesScreen
import co.minilora.app.ui.screens.HelpScreen
import co.minilora.app.ui.screens.RemoteScreen
import co.minilora.app.ui.screens.TrackerScreen

/** Top-level destinations. */
enum class Tab(val label: String, val icon: ImageVector) {
    TRACKER("Map", Icons.Filled.Map),
    SCAN("Devices", Icons.Filled.BluetoothSearching),
    REMOTE("Remote", Icons.Filled.SettingsRemote),
    CONFIG("Setup", Icons.Filled.Tune),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MiniLoRaApp(vm: AppViewModel, onOpenSettings: () -> Unit,
                onOpenHelp: () -> Unit) {
    var tab by remember { mutableStateOf(Tab.TRACKER) }
    val snackHost = remember { SnackbarHostState() }
    val snack by vm.snack.collectAsState()

    LaunchedEffect(snack) {
        snack?.let {
            snackHost.showSnackbar(it)
            vm.consumeSnack()
        }
    }

    Scaffold(
        floatingActionButton = {
            // The beacon scanner is manual: the FAB toggles the BLE scan
            // (vm.toggleScan was never wired to any UI before v2.6.1).
            if (tab == Tab.SCAN) {
                FloatingActionButton(onClick = { vm.toggleScan() }) {
                    Icon(
                        if (vm.scanActive.collectAsState().value)
                            Icons.Filled.BluetoothDisabled
                        else Icons.Filled.BluetoothSearching,
                        contentDescription = "Start/stop BLE scan",
                    )
                }
            }
        },
        topBar = {
            TopAppBar(
                title = { Text("MiniLoRa") },
                actions = {
                    IconButton(onClick = onOpenHelp) {
                        Icon(Icons.Filled.HelpOutline, contentDescription = "Help & guide")
                    }
                    IconButton(onClick = onOpenSettings) {
                        Icon(Icons.Filled.Settings, contentDescription = "Settings")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackHost) },
        bottomBar = {
            NavigationBar {
                Tab.entries.forEach { t ->
                    NavigationBarItem(
                        selected = tab == t,
                        onClick = { tab = t },
                        icon = { Icon(t.icon, contentDescription = t.label) },
                        alwaysShowLabel = true,
                        label = { Text(t.label) },
                    )
                }
            }
        },
    ) { padding ->
        val mod = Modifier.padding(padding)
        when (tab) {
            Tab.TRACKER -> TrackerScreen(vm, mod)
            Tab.SCAN -> DevicesScreen(vm, mod)
            Tab.REMOTE -> RemoteScreen(vm, mod)
            Tab.CONFIG -> ConfigScreen(vm, mod)
        }
    }
}
