package co.minilora.app.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/**
 * In-app guide: what MiniLoRa is, what the tabs do, the radio modes and
 * how to provision — so the app is usable in the field without the repo
 * docs. Facts mirror the ICD and the firmware behavior (2-min config
 * window, ~16-boot diag cadence, trek contact-lost LED, …).
 */
@Composable
fun HelpScreen(onDone: () -> Unit, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .systemBarsPadding()
            .imePadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(Modifier.fillMaxWidth().padding(vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onDone) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
            }
            Text("Help & guide", style = MaterialTheme.typography.titleLarge)
        }
        Text(
            "Everything below also applies offline — the app and the " +
                "devices work with no internet unless you configure TTN " +
                "or a server yourself.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)

        HelpSection("What is MiniLoRa?", defaultExpanded = true) {
            Text("MiniLoRa is a small ecosystem of battery-powered tracker " +
                "devices and this app:", style = MaterialTheme.typography.bodyMedium)
            HelpBullet("The device: an ESP32-C6 with a LoRa radio (SX1262), " +
                "GPS, temperature/humidity/pressure (BME280), light (OPT3001) " +
                "and motion (LIS3DH) sensors. It sleeps deeply, wakes on a " +
                "schedule (the sleep table), reads its sensors, sends one " +
                "frame and goes back to sleep — that is why battery life is " +
                "measured in weeks.")
            HelpBullet("The app: receives the device's BLE beacon directly " +
                "(a few meters), pulls from The Things Network (TTN, if you " +
                "use LoRaWAN) and from your own MiniLoRa server on the LAN " +
                "(if the device pushes over WiFi).")
            HelpBullet("The server (optional): a small service on your own " +
                "machine that stores history, draws dashboards and forwards " +
                "P2P gateway traffic. The app and the devices work fine " +
                "without it.")
        }

        HelpSection("The four tabs") {
            HelpBullet("Map — the home screen: current conditions, the " +
                "position track (one color per device), the payload history " +
                "strip and, in trek mode, the peer contact card.")
            HelpBullet("Devices — live BLE scan. Every MiniLoRa beacon in " +
                "range is listed; tap one to make it the target for Setup " +
                "and status reads.")
            HelpBullet("Remote — commands to the device over TTN downlink " +
                "(locate now, send diag, reset counters, open BLE window, " +
                "firmware update), the diagnostics dashboard and remote " +
                "config toggles.")
            HelpBullet("Setup — provision a device over BLE: radio mode, " +
                "keys, WiFi, sensors, power. Read from the device first, " +
                "edit, then Write to device.")
        }

        HelpSection("Radio modes") {
            Text("LoRaWAN (default)", style = MaterialTheme.typography.titleSmall)
            Text("The device talks to The Things Network like any LoRaWAN " +
                "node. You need the JoinEUI, DevEUI, AppKey and NwkKey from " +
                "your TTN console. The app pulls your data through the TTN " +
                "API (Settings). Server coverage is wherever TTN gateways " +
                "are.",
                style = MaterialTheme.typography.bodySmall)
            Text("P2P star (no cloud)", style = MaterialTheme.typography.titleSmall)
            Text("One powered device acts as the gateway: leaves transmit " +
                "raw LoRa to it on a private channel, and the gateway " +
                "forwards their frames to your LAN server (where it has " +
                "WiFi). Works with zero subscriptions; range is whatever " +
                "the air profile allows (SF12 = kilometers). All devices " +
                "share the mesh key — that key IS the network.",
                style = MaterialTheme.typography.bodySmall)
            Text("Trek (two devices, nothing else)", style = MaterialTheme.typography.titleSmall)
            Text("Two devices exchange their positions directly at every " +
                "wake, on a GPS-anchored schedule (the sleep table is the " +
                "exchange period). Each device re-broadcasts the other's " +
                "position over BLE, so BOTH phones show BOTH tracks — " +
                "yours in blue, your peer's in green. No gateway, no " +
                "server, no TTN. Pairing = setting the same mesh key, " +
                "profile and period on both devices.",
                style = MaterialTheme.typography.bodySmall)
            Text("Trek details worth knowing:", style = MaterialTheme.typography.labelLarge)
            HelpBullet("First contact takes 1–3 windows (the devices try " +
                "random roles until they hear each other, then lock " +
                "id-order roles).")
            HelpBullet("If the link weakens, both radios step their power " +
                "up (14 → 17 → 20 → 22 dBm, legal on 869.525 MHz) and " +
                "step back down when the link is solid again.")
            HelpBullet("3 missed windows → the green LED plays a 3× " +
                "double-blink at each wake, and the app's contact card " +
                "shows CONTACT LOST. Any received frame clears it.")
        }

        HelpSection("Provisioning a device (step by step)", defaultExpanded = false) {
            HelpText("1. Wake the device into config mode: hold its button " +
                "~5 s. The LED signals the config window, which stays " +
                "open for 2 minutes. (You can also open it remotely: " +
                "Remote → Open BLE window — it rides the next wake.)")
            HelpText("2. Devices tab: the device appears (its config " +
                "advertisement is listed even with the beacon off). Tap it.")
            HelpText("3. Setup tab: Read from device — this loads its " +
                "current config into the editor.")
            HelpText("4. Edit what you need. The radio-mode card explains " +
                "each mode and only shows the fields that mode needs.")
            HelpText("5. Write to device — the config travels as a " +
                "CRC-protected envelope and applies at the device's next " +
                "wake. Nothing is written until you press Write.")
            HelpText("Export .cfg / Import .cfg save and load that same " +
                "envelope as a file — handy for cloning a device or " +
                "keeping a backup of your keys.")
        }

        HelpSection("Data feeds & the history strip") {
            HelpBullet("BLE: while the app is open, a battery-balanced " +
                "scan listens for every MiniLoRa beacon in range — no " +
                "configuration needed.")
            HelpBullet("TTN: polled on your interval from Settings; also " +
                "covers time the app was closed (recent frames backfill).")
            HelpBullet("WiFi server: polled from your LAN server the same " +
                "way.")
            HelpBullet("Frames from different feeds that belong to the " +
                "same device wake merge into ONE history card showing all " +
                "sources (BLE+TTN+WiFi).")
            HelpBullet("The filter chips above the map select a device " +
                "or group; the color of each chip's dot matches its track " +
                "on the map.")
        }

        HelpSection("Remote commands & diagnostics") {
            HelpBullet("LoRaWAN is class A: a command you queue is " +
                "delivered during the RX window of the device's NEXT " +
                "uplink and takes effect the cycle after that. The Remote " +
                "tab shows the estimated effect time.")
            HelpBullet("A newer pending command replaces an older one " +
                "(there is one downlink slot per wake). Clear TTN queue " +
                "cancels pending commands.")
            HelpBullet("The diagnostics frame arrives every ~16 boots or " +
                "on demand (Send diag): battery, GPS timeouts, join/send " +
                "failures, reset reasons, TTFF and more.")
            HelpBullet("Remote config toggles (WiFi, Zigbee, BLE beacon, " +
                "GNSS, sensors…) are saved in the device's NVS and " +
                "confirmed by the next diag frame.")
        }

        HelpSection("Device signals & recovery") {
            HelpBullet("Config window: LED signals while the device " +
                "advertises BLE config for 2 minutes (button long-press, " +
                "recovery mode, or the remote BLE window command).")
            HelpBullet("Trek contact lost: green LED 3× double-blink at " +
                "each wake after 3 missed exchange windows.")
            HelpBullet("Recovery mode: after repeated unexpected resets " +
                "the device ships a minimal cycle (config window only, no " +
                "radio TX) so it can always be re-provisioned. The diag " +
                "flags show RECOVERY.")
            HelpBullet("Battery flags: LOW BATTERY doubles the wake " +
                "period; CRITICAL pins it to the table maximum and skips " +
                "extras. Both appear on the diag card and the Map header.")
        }

        HelpSection("Troubleshooting") {
            HelpBullet("Device invisible on Devices tab: it only beacons " +
                "while awake — wake it, or open the config window (5 s " +
                "button hold). Keep the phone close; the beacon uses BLE " +
                "coded PHY (good range, but not through buildings).")
            HelpBullet("No data on the Map: check that at least one feed " +
                "is configured and polling (Settings → Polling), and that " +
                "the TTN device id matches.")
            HelpBullet("GPS no fix: cold starts can take minutes with a " +
                "clear sky view — indoors it may never fix. The frame " +
                "ships an honest \"no fix\" and the position is skipped.")
            HelpBullet("Trek: no contact — verify both devices have the " +
                "SAME mesh key, profile and period, and that each got a " +
                "GPS fix (the schedule anchors to GPS time). Expect the " +
                "first contact within 1–3 windows.")
            HelpBullet("Setup write failed: the device left the config " +
                "window (2 min) — press the button again and retry.")
        }

        HelpSection("Frequency, power & legality") {
            HelpBullet("All P2P profiles use 869.525 MHz (EU 869.4–869.65 " +
                "MHz band) with a private sync word — invisible to " +
                "LoRaWAN networks.")
            HelpBullet("The power ladder tops out at +22 dBm, comfortably " +
                "inside the band's 27 dBm ERP limit. The default is " +
                "+14 dBm.")
            HelpBullet("Firmware updates travel over WiFi or USB only — " +
                "never over LoRa/P2P.")
        }

        HelpSection("Privacy & data") {
            HelpBullet("Your data goes exactly where you configure it: " +
                "TTN (if configured) and your own server (if configured). " +
                "The BLE path never leaves the phone.")
            HelpBullet("The settings export file contains your TTN API " +
                "key in clear text — treat it like a credential.")
            HelpBullet("The P2P mesh key is a shared secret: anyone " +
                "holding it can send frames your devices accept, and a " +
                "stolen device can be read. Fine for a personal fleet; " +
                "don't use a key you also use elsewhere.")
            HelpBullet("Position history is stored on the phone (with a " +
                "prune tool on the Map tab) and, if configured, on your " +
                "own server.")
        }

        Text(
            "Firmware 3.0 · P2P star + trek per ICD §10 · this guide " +
                "ships with the app, so it matches the installed version.",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(vertical = 8.dp))
    }
}

/** An expandable titled section (collapsed by default except the first). */
@Composable
private fun HelpSection(title: String, defaultExpanded: Boolean = false,
                        content: @Composable ColumnScope.() -> Unit) {
    var expanded by remember { mutableStateOf(defaultExpanded) }
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth()
                .clickable { expanded = !expanded }
                .padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(title, Modifier.weight(1f),
                style = MaterialTheme.typography.titleSmall)
            Icon(if (expanded) Icons.Filled.ExpandLess
                 else Icons.Filled.ExpandMore,
                 contentDescription = if (expanded) "Collapse" else "Expand")
        }
        AnimatedVisibility(expanded) {
            Column(Modifier.padding(start = 12.dp, end = 12.dp, bottom = 12.dp),
                   verticalArrangement = Arrangement.spacedBy(6.dp)) {
                content()
            }
        }
    }
}

@Composable
private fun HelpText(text: String) {
    Text(text, style = MaterialTheme.typography.bodySmall)
}

@Composable
private fun HelpBullet(text: String) {
    Text("•  $text", style = MaterialTheme.typography.bodySmall)
}
