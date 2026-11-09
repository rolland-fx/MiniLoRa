package co.minilora.app.util

import android.content.Context
import android.util.Log
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import java.net.InetAddress
import java.io.File
import java.io.FileReader
import java.io.FileWriter
import java.util.Properties
import org.json.JSONObject

object DeviceConfig {
    private const val PREFS_NAME = "minilora_prefs"
    private const val KEY_DEVICE_NAME = "target_device_name"
    private const val KEY_DEVICE_MAC = "target_device_mac"

    // TTN config keys
    private const val KEY_TTN_API_KEY = "ttn_api_key"
    private const val KEY_TTN_APP_ID = "ttn_app_id"
    private const val KEY_TTN_DEVICE_ID = "ttn_device_id"
    private const val KEY_TTN_POLL_INTERVAL = "ttn_poll_interval"
    private const val KEY_TTN_REGION = "ttn_region"

    // WiFi / local server config keys
    private const val KEY_WIFI_SERVER = "wifi_server_ip"
    private const val KEY_WIFI_PORT = "wifi_server_port"
    private const val KEY_WIFI_POLL_INTERVAL = "wifi_poll_interval"

    // BLE beacon polling keys (see AppViewModel.blePollLoop)
    private const val KEY_BLE_POLLING = "ble_polling_enabled"
    private const val KEY_BLE_POLL_INTERVAL = "ble_poll_interval"
    private const val KEY_TTN_POLLING = "ttn_polling_enabled"
    private const val KEY_WIFI_POLLING = "wifi_polling_enabled"

    // Multi-tracker: extra TTN device ids + the identity->group map
    private const val KEY_TTN_DEVICE_IDS = "ttn_device_ids"
    private const val KEY_DEVICE_GROUPS = "device_group_map"

    // Device config (BLE characteristic fields)
    private const val KEY_DEV_GLOBAL_NAME = "dev_global_name"
    private const val KEY_DEV_WIFI_SSID = "dev_wifi_ssid"
    private const val KEY_DEV_WIFI_PASSWORD = "dev_wifi_password"
    private const val KEY_DEV_WIFI_SERVER_IP = "dev_wifi_server_ip"
    private const val KEY_DEV_WIFI_SERVER_PORT = "dev_wifi_server_port"
    private const val KEY_DEV_JOIN_EUI = "dev_join_eui"
    private const val KEY_DEV_DEV_EUI = "dev_dev_eui"
    private const val KEY_DEV_APP_KEY = "dev_app_key"
    private const val KEY_DEV_NWK_KEY = "dev_nwk_key"
    private const val KEY_DEV_CFG_LORAWAN = "dev_cfg_lorawan"
    private const val KEY_DEV_CFG_WIFI = "dev_cfg_wifi"
    private const val KEY_DEV_CFG_GNSS = "dev_cfg_gnss"
    private const val KEY_DEV_CFG_BME280 = "dev_cfg_bme280"
    private const val KEY_DEV_CFG_OPT30 = "dev_cfg_opt30"
    private const val KEY_DEV_CFG_LIS3DH = "dev_cfg_lis3dh"
    private const val KEY_DEV_CFG_BLE = "dev_cfg_ble"
    private const val KEY_DEV_CFG_ZIGBEE = "dev_cfg_zigbee"
    private const val KEY_DEV_CFG_RANGE = "dev_cfg_range"
    private const val KEY_DEV_CFG_ASSET = "dev_cfg_asset"
    private const val KEY_DEV_CFG_MOTION_SENS = "dev_cfg_motion_sens"
    private const val KEY_DEV_CFG_ASSET_HB_H = "dev_cfg_asset_hb_h"
    private const val KEY_DEV_CFG_BURST_MIN = "dev_cfg_burst_min"
    private const val KEY_DEV_CFG_BURST_CAP = "dev_cfg_burst_cap"
    private const val KEY_DEV_CFG_P2P = "dev_cfg_p2p"
    private const val KEY_DEV_CFG_P2P_GW = "dev_cfg_p2p_gateway"
    private const val KEY_DEV_CFG_P2P_PROFILE = "dev_cfg_p2p_profile"
    private const val KEY_DEV_CFG_P2P_RX = "dev_cfg_p2p_rx_after_tx"
    private const val KEY_DEV_CFG_P2P_ID = "dev_cfg_p2p_short_id"
    private const val KEY_DEV_CFG_P2P_KEY = "dev_cfg_p2p_mesh_key"

    var targetDeviceName: String = "MiniLoRa"
    var targetMacAddress: String = ""

    // TTN settings
    var ttnApiKey: String = ""
    var ttnAppId: String = ""
    var ttnDeviceId: String = ""
    var ttnPollIntervalSeconds: Int = 300
    var ttnRegion: String = "eu1"

    // WiFi / local server settings
    var wifiServerIp: String = ""
    var wifiServerPort: Int = 5000
    var wifiPollIntervalSeconds: Int = 300

    // Polling auto-resume (the switches live in Settings — app behavior,
    // not device control; remembered across restarts like the BLE one)
    var ttnPollingEnabled: Boolean = false
    var wifiPollingEnabled: Boolean = false

    // BLE beacon polling (see AppViewModel.blePollLoop). Default ON: the
    // tracker beacons its telemetry whether or not any feed is configured,
    // and the app is expected to pull it whenever it is open.
    var blePollingEnabled: Boolean = true
    var blePollIntervalSeconds: Int = 60

    // Additional TTN device ids to poll (multi-tracker): the primary stays
    // ttnDeviceId (remote commands + the Settings field); every id here is
    // also fetched and tagged with its own identity.
    var ttnDeviceIds: List<String> = emptyList()

    // identity -> display group label, learned from the configured
    // MiniLoRa-server's /api/devices ([device_groups]) — a tracker heard
    // as a BLE MAC, a TTN id and a WiFi name then shows as ONE card.
    var deviceGroupMap: Map<String, String> = emptyMap()

    // Device BLE configuration fields (for writing to the MiniLoRa device)
    var deviceGlobalName: String = "MiniLoRa"
    var deviceWifiSSID: String = ""
    var deviceWifiPassword: String = ""
    var deviceWifiServerIP: String = ""
    var deviceWifiServerPort: Int = 80
    var deviceJoinEUI: String = ""
    var deviceDevEUI: String = ""
    var deviceAppKey: String = ""
    var deviceNwkKey: String = ""
    var deviceCfgLoRaWAN: Boolean = false
    var deviceCfgWiFi: Boolean = false
    var deviceCfgGNSS: Boolean = true
    var deviceCfgBME280: Boolean = true
    var deviceCfgOPT30: Boolean = true
    var deviceCfgLIS3DH: Boolean = true
    var deviceCfgBLE: Boolean = true
    var deviceCfgZigbee: Boolean = false
    var deviceCfgRange: Int = 0
    var deviceCfgAssetMode: Boolean = false
    var deviceCfgMotionSens: Int = 2
    var deviceCfgAssetHeartbeatH: Int = 24
    var deviceCfgBurstIntervalMin: Int = 5
    var deviceCfgBurstMaxPerDay: Int = 20
    // P2P star (ICD §10) — editor-side mirror of the schema-5 envelope
    var deviceCfgP2P: Boolean = false
    var deviceCfgP2PGateway: Boolean = false
    var deviceCfgP2PProfile: Int = 0
    var deviceCfgP2PRxAfterTx: Boolean = true
    var deviceCfgP2PShortId: String = ""
    var deviceCfgMeshKey: String = ""

    private var prefs: android.content.SharedPreferences? = null

    fun init(context: Context) {
        val masterKey = MasterKey.Builder(context)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        prefs = EncryptedSharedPreferences.create(
            context,
            PREFS_NAME,
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
        load()
    }

    private fun load() {
        prefs?.let { p ->
            val savedName = p.getString(KEY_DEVICE_NAME, "")
            val savedMac = p.getString(KEY_DEVICE_MAC, "")
            if (savedName.isNullOrEmpty().not()) {
                targetDeviceName = savedName
            }
            if (savedMac.isNullOrEmpty().not()) {
                targetMacAddress = savedMac
            }

            // TTN settings
            ttnApiKey = p.getString(KEY_TTN_API_KEY, "") ?: ""
            ttnAppId = p.getString(KEY_TTN_APP_ID, "") ?: ""
            ttnDeviceId = p.getString(KEY_TTN_DEVICE_ID, "") ?: ""
            ttnPollIntervalSeconds = p.getInt(KEY_TTN_POLL_INTERVAL, 300)
            ttnRegion = p.getString(KEY_TTN_REGION, "eu1") ?: "eu1"

            // WiFi / local server settings
            wifiServerIp = p.getString(KEY_WIFI_SERVER, "") ?: ""
            wifiServerPort = p.getInt(KEY_WIFI_PORT, 5000)
            wifiPollIntervalSeconds = p.getInt(KEY_WIFI_POLL_INTERVAL, 300)

            // BLE beacon polling — default ON (the app pulls whenever open)
            blePollingEnabled = p.getBoolean(KEY_BLE_POLLING, true)
            blePollIntervalSeconds = p.getInt(KEY_BLE_POLL_INTERVAL, 60)

            // Polling auto-resume. TTN/WiFi default ON: installs upgrading
            // from the session-only era expected their feeds to keep
            // running (the key is absent on upgrade, not false — an
            // explicit OFF is stored by the toggle and respected).
            ttnPollingEnabled = p.getBoolean(KEY_TTN_POLLING, true)
            wifiPollingEnabled = p.getBoolean(KEY_WIFI_POLLING, true)

            // Multi-tracker: extra TTN ids + the identity->group map
            ttnDeviceIds = run {
                val s = p.getString(KEY_TTN_DEVICE_IDS, "") ?: ""
                if (s.isBlank()) emptyList()
                else try {
                    val arr = org.json.JSONArray(s)
                    (0 until arr.length()).map { arr.optString(it) }.filter { it.isNotBlank() }
                } catch (_: Exception) { emptyList() }
            }
            deviceGroupMap = run {
                val s = p.getString(KEY_DEVICE_GROUPS, "") ?: ""
                if (s.isBlank()) emptyMap()
                else try {
                    val o = org.json.JSONObject(s)
                    o.keys().asSequence().filter { o.optString(it).isNotBlank() }
                        .associateWith { o.optString(it) }
                } catch (_: Exception) { emptyMap() }
            }

            // Device BLE configuration
            deviceGlobalName = p.getString(KEY_DEV_GLOBAL_NAME, "MiniLoRa") ?: "MiniLoRa"
            deviceWifiSSID = p.getString(KEY_DEV_WIFI_SSID, "") ?: ""
            deviceWifiPassword = p.getString(KEY_DEV_WIFI_PASSWORD, "") ?: ""
            deviceWifiServerIP = p.getString(KEY_DEV_WIFI_SERVER_IP, "") ?: ""
            deviceWifiServerPort = p.getInt(KEY_DEV_WIFI_SERVER_PORT, 80)
            deviceJoinEUI = p.getString(KEY_DEV_JOIN_EUI, "") ?: ""
            deviceDevEUI = p.getString(KEY_DEV_DEV_EUI, "") ?: ""
            deviceAppKey = p.getString(KEY_DEV_APP_KEY, "") ?: ""
            deviceNwkKey = p.getString(KEY_DEV_NWK_KEY, "") ?: ""
            deviceCfgLoRaWAN = p.getBoolean(KEY_DEV_CFG_LORAWAN, false)
            deviceCfgWiFi = p.getBoolean(KEY_DEV_CFG_WIFI, false)
            deviceCfgGNSS = p.getBoolean(KEY_DEV_CFG_GNSS, true)
            deviceCfgBME280 = p.getBoolean(KEY_DEV_CFG_BME280, true)
            deviceCfgOPT30 = p.getBoolean(KEY_DEV_CFG_OPT30, true)
            deviceCfgLIS3DH = p.getBoolean(KEY_DEV_CFG_LIS3DH, true)
            deviceCfgBLE = p.getBoolean(KEY_DEV_CFG_BLE, true)
            deviceCfgZigbee = p.getBoolean(KEY_DEV_CFG_ZIGBEE, false)
            deviceCfgRange = p.getInt(KEY_DEV_CFG_RANGE, 0)
            deviceCfgAssetMode = p.getBoolean(KEY_DEV_CFG_ASSET, false)
            deviceCfgMotionSens = p.getInt(KEY_DEV_CFG_MOTION_SENS, 2)
            deviceCfgAssetHeartbeatH = p.getInt(KEY_DEV_CFG_ASSET_HB_H, 24)
            deviceCfgBurstIntervalMin = p.getInt(KEY_DEV_CFG_BURST_MIN, 5)
            deviceCfgBurstMaxPerDay = p.getInt(KEY_DEV_CFG_BURST_CAP, 20)
            deviceCfgP2P = p.getBoolean(KEY_DEV_CFG_P2P, false)
            deviceCfgP2PGateway = p.getBoolean(KEY_DEV_CFG_P2P_GW, false)
            deviceCfgP2PProfile = p.getInt(KEY_DEV_CFG_P2P_PROFILE, 0)
            deviceCfgP2PRxAfterTx = p.getBoolean(KEY_DEV_CFG_P2P_RX, true)
            deviceCfgP2PShortId = p.getString(KEY_DEV_CFG_P2P_ID, "") ?: ""
            deviceCfgMeshKey = p.getString(KEY_DEV_CFG_P2P_KEY, "") ?: ""

            Log.d("DeviceConfig",
                "BLE: name='$targetDeviceName' mac='$targetMacAddress' " +
                "TTN: app='$ttnAppId' device='$ttnDeviceId' apiKey=${ttnApiKey.take(8)}... " +
                "WiFi: server='$wifiServerIp' poll=${wifiPollIntervalSeconds}s")
        }
    }

    fun save() {
        prefs?.edit()
            ?.putString(KEY_DEVICE_NAME, targetDeviceName)
            ?.putString(KEY_DEVICE_MAC, targetMacAddress)
            ?.putString(KEY_TTN_API_KEY, ttnApiKey)
            ?.putString(KEY_TTN_APP_ID, ttnAppId)
            ?.putString(KEY_TTN_DEVICE_ID, ttnDeviceId)
            ?.putInt(KEY_TTN_POLL_INTERVAL, ttnPollIntervalSeconds)
            ?.putString(KEY_TTN_REGION, ttnRegion)
            ?.putString(KEY_WIFI_SERVER, wifiServerIp)
            ?.putInt(KEY_WIFI_PORT, wifiServerPort)
            ?.putInt(KEY_WIFI_POLL_INTERVAL, wifiPollIntervalSeconds)
            ?.putBoolean(KEY_BLE_POLLING, blePollingEnabled)
            ?.putInt(KEY_BLE_POLL_INTERVAL, blePollIntervalSeconds)
            ?.putString(KEY_TTN_DEVICE_IDS,
                org.json.JSONArray(ttnDeviceIds).toString())
            ?.putString(KEY_DEVICE_GROUPS,
                org.json.JSONObject(deviceGroupMap).toString())
            ?.putBoolean(KEY_TTN_POLLING, ttnPollingEnabled)
            ?.putBoolean(KEY_WIFI_POLLING, wifiPollingEnabled)
            // Device BLE config
            ?.putString(KEY_DEV_GLOBAL_NAME, deviceGlobalName)
            ?.putString(KEY_DEV_WIFI_SSID, deviceWifiSSID)
            ?.putString(KEY_DEV_WIFI_PASSWORD, deviceWifiPassword)
            ?.putString(KEY_DEV_WIFI_SERVER_IP, deviceWifiServerIP)
            ?.putInt(KEY_DEV_WIFI_SERVER_PORT, deviceWifiServerPort)
            ?.putString(KEY_DEV_JOIN_EUI, deviceJoinEUI)
            ?.putString(KEY_DEV_DEV_EUI, deviceDevEUI)
            ?.putString(KEY_DEV_APP_KEY, deviceAppKey)
            ?.putString(KEY_DEV_NWK_KEY, deviceNwkKey)
            ?.putBoolean(KEY_DEV_CFG_LORAWAN, deviceCfgLoRaWAN)
            ?.putBoolean(KEY_DEV_CFG_WIFI, deviceCfgWiFi)
            ?.putBoolean(KEY_DEV_CFG_GNSS, deviceCfgGNSS)
            ?.putBoolean(KEY_DEV_CFG_BME280, deviceCfgBME280)
            ?.putBoolean(KEY_DEV_CFG_OPT30, deviceCfgOPT30)
            ?.putBoolean(KEY_DEV_CFG_LIS3DH, deviceCfgLIS3DH)
            ?.putBoolean(KEY_DEV_CFG_BLE, deviceCfgBLE)
            ?.putBoolean(KEY_DEV_CFG_ZIGBEE, deviceCfgZigbee)
            ?.putInt(KEY_DEV_CFG_RANGE, deviceCfgRange)
            ?.putBoolean(KEY_DEV_CFG_ASSET, deviceCfgAssetMode)
            ?.putInt(KEY_DEV_CFG_MOTION_SENS, deviceCfgMotionSens)
            ?.putInt(KEY_DEV_CFG_ASSET_HB_H, deviceCfgAssetHeartbeatH)
            ?.putInt(KEY_DEV_CFG_BURST_MIN, deviceCfgBurstIntervalMin)
            ?.putInt(KEY_DEV_CFG_BURST_CAP, deviceCfgBurstMaxPerDay)
            ?.putBoolean(KEY_DEV_CFG_P2P, deviceCfgP2P)
            ?.putBoolean(KEY_DEV_CFG_P2P_GW, deviceCfgP2PGateway)
            ?.putInt(KEY_DEV_CFG_P2P_PROFILE, deviceCfgP2PProfile)
            ?.putBoolean(KEY_DEV_CFG_P2P_RX, deviceCfgP2PRxAfterTx)
            ?.putString(KEY_DEV_CFG_P2P_ID, deviceCfgP2PShortId)
            ?.putString(KEY_DEV_CFG_P2P_KEY, deviceCfgMeshKey)
            ?.apply()
        Log.d("DeviceConfig", "Saved all config")
    }

    // ---- global settings file (JSON) ----
    // Covers the APP's own settings: target device, TTN (region/app/key/
    // device/poll) and the local WiFi server. Device BLE fields are NOT
    // here — those travel in the .cfg envelope (Config tab). The file
    // contains the TTN API key: treat it like a credential.

    /** Global app settings as a versioned JSON document. */
    fun exportSettingsJson(): String {
        val root = JSONObject()
        root.put("type", "minilora-app-settings")
        root.put("version", 1)
        root.put("targetDeviceName", targetDeviceName)
        root.put("targetMacAddress", targetMacAddress)
        root.put("ttn", JSONObject()
            .put("region", ttnRegion)
            .put("appId", ttnAppId)
            .put("deviceId", ttnDeviceId)
            .put("apiKey", ttnApiKey)
            .put("pollIntervalSeconds", ttnPollIntervalSeconds))
        root.put("wifi", JSONObject()
            .put("serverIp", wifiServerIp)
            .put("serverPort", wifiServerPort)
            .put("pollIntervalSeconds", wifiPollIntervalSeconds))
        return root.toString(2)
    }

    /**
     * Restore global settings from exportSettingsJson() output.
     * Unknown keys are ignored (forward-compatible); missing keys keep the
     * current values. Throws on foreign documents. Persists on success.
     */
    fun importSettingsJson(json: String) {
        val root = JSONObject(json)
        if (root.optString("type") != "minilora-app-settings") {
            throw IllegalArgumentException("not a MiniLoRa app-settings file")
        }
        val version = root.optInt("version", 1)
        if (version != 1) {
            throw IllegalArgumentException("unsupported settings version $version")
        }
        targetDeviceName = root.optString("targetDeviceName", targetDeviceName)
        targetMacAddress = root.optString("targetMacAddress", targetMacAddress)
        root.optJSONObject("ttn")?.let { t ->
            ttnRegion = t.optString("region", ttnRegion)
            ttnAppId = t.optString("appId", ttnAppId)
            ttnDeviceId = t.optString("deviceId", ttnDeviceId)
            ttnApiKey = t.optString("apiKey", ttnApiKey)
            ttnPollIntervalSeconds =
                t.optInt("pollIntervalSeconds", ttnPollIntervalSeconds)
        }
        root.optJSONObject("wifi")?.let { w ->
            wifiServerIp = w.optString("serverIp", wifiServerIp)
            wifiServerPort = w.optInt("serverPort", wifiServerPort)
                .coerceIn(1, 65535)
            wifiPollIntervalSeconds =
                w.optInt("pollIntervalSeconds", wifiPollIntervalSeconds)
        }
        save()
    }

    /** Whether TTN fetch is configured (has API key + app ID + device ID) */
    fun ttnConfigured(): Boolean {
        return ttnApiKey.isNotBlank() && ttnAppId.isNotBlank() && ttnDeviceId.isNotBlank()
    }    /** Whether WiFi / local server is configured (has valid server IP) */
    fun wifiConfigured(): Boolean {
        return isValidIpAddress(wifiServerIp)
    }

    /** Validates that a string is a syntactically valid IPv4 address */
    fun isValidIpAddress(ip: String): Boolean {
        if (ip.isBlank()) return false
        val parts = ip.split(".")
        if (parts.size != 4) return false
        for (part in parts) {
            val num = part.toIntOrNull() ?: return false
            if (num < 0 || num > 255) return false
        }
        return true
    }

    /** Export all device BLE config fields to a properties file */
    fun exportToFile(file: File) {
        val props = Properties()
        props.setProperty("deviceGlobalName", deviceGlobalName)
        props.setProperty("deviceWifiSSID", deviceWifiSSID)
        props.setProperty("deviceWifiPassword", deviceWifiPassword)
        props.setProperty("deviceWifiServerIP", deviceWifiServerIP)
        props.setProperty("deviceWifiServerPort", deviceWifiServerPort.toString())
        props.setProperty("deviceJoinEUI", deviceJoinEUI)
        props.setProperty("deviceDevEUI", deviceDevEUI)
        props.setProperty("deviceAppKey", deviceAppKey)
        props.setProperty("deviceNwkKey", deviceNwkKey)
        props.setProperty("deviceCfgLoRaWAN", deviceCfgLoRaWAN.toString())
        props.setProperty("deviceCfgWiFi", deviceCfgWiFi.toString())
        props.setProperty("deviceCfgGNSS", deviceCfgGNSS.toString())
        props.setProperty("deviceCfgBME280", deviceCfgBME280.toString())
        props.setProperty("deviceCfgOPT30", deviceCfgOPT30.toString())
        props.setProperty("deviceCfgLIS3DH", deviceCfgLIS3DH.toString())
        props.setProperty("deviceCfgBLE", deviceCfgBLE.toString())
        props.setProperty("deviceCfgZigbee", deviceCfgZigbee.toString())
        props.setProperty("deviceCfgRange", deviceCfgRange.toString())
        props.setProperty("deviceCfgAssetMode", deviceCfgAssetMode.toString())
        props.setProperty("deviceCfgMotionSens", deviceCfgMotionSens.toString())
        props.setProperty("deviceCfgAssetHeartbeatH", deviceCfgAssetHeartbeatH.toString())
        props.setProperty("deviceCfgBurstIntervalMin", deviceCfgBurstIntervalMin.toString())
        props.setProperty("deviceCfgBurstMaxPerDay", deviceCfgBurstMaxPerDay.toString())
        props.setProperty("deviceCfgP2P", deviceCfgP2P.toString())
        props.setProperty("deviceCfgP2PGateway", deviceCfgP2PGateway.toString())
        props.setProperty("deviceCfgP2PProfile", deviceCfgP2PProfile.toString())
        props.setProperty("deviceCfgP2PRxAfterTx", deviceCfgP2PRxAfterTx.toString())
        props.setProperty("deviceCfgP2PShortId", deviceCfgP2PShortId)
        props.setProperty("deviceCfgMeshKey", deviceCfgMeshKey)
        FileWriter(file).use { props.store(it, "MiniLoRa Device Config") }
    }

    /** Import device BLE config fields from a properties file */
    fun importFromFile(file: File) {
        val props = Properties()
        FileReader(file).use { props.load(it) }
        deviceGlobalName = props.getProperty("deviceGlobalName", "MiniLoRa")
        deviceWifiSSID = props.getProperty("deviceWifiSSID", "")
        deviceWifiPassword = props.getProperty("deviceWifiPassword", "")
        deviceWifiServerIP = props.getProperty("deviceWifiServerIP", "")
        deviceWifiServerPort = props.getProperty("deviceWifiServerPort", "80").toIntOrNull() ?: 80
        deviceJoinEUI = props.getProperty("deviceJoinEUI", "")
        deviceDevEUI = props.getProperty("deviceDevEUI", "")
        deviceAppKey = props.getProperty("deviceAppKey", "")
        deviceNwkKey = props.getProperty("deviceNwkKey", "")
        deviceCfgLoRaWAN = props.getProperty("deviceCfgLoRaWAN", "false").toBoolean()
        deviceCfgWiFi = props.getProperty("deviceCfgWiFi", "false").toBoolean()
        deviceCfgGNSS = props.getProperty("deviceCfgGNSS", "true").toBoolean()
        deviceCfgBME280 = props.getProperty("deviceCfgBME280", "true").toBoolean()
        deviceCfgOPT30 = props.getProperty("deviceCfgOPT30", "true").toBoolean()
        deviceCfgLIS3DH = props.getProperty("deviceCfgLIS3DH", "true").toBoolean()
        deviceCfgBLE = props.getProperty("deviceCfgBLE", "true").toBoolean()
        deviceCfgZigbee = props.getProperty("deviceCfgZigbee", "false").toBoolean()
        deviceCfgRange = props.getProperty("deviceCfgRange", "0").toIntOrNull() ?: 0
        deviceCfgAssetMode = props.getProperty("deviceCfgAssetMode", "false").toBoolean()
        deviceCfgMotionSens = props.getProperty("deviceCfgMotionSens", "2").toIntOrNull() ?: 2
        deviceCfgAssetHeartbeatH = props.getProperty("deviceCfgAssetHeartbeatH", "24").toIntOrNull() ?: 24
        deviceCfgBurstIntervalMin = props.getProperty("deviceCfgBurstIntervalMin", "5").toIntOrNull() ?: 5
        deviceCfgBurstMaxPerDay = props.getProperty("deviceCfgBurstMaxPerDay", "20").toIntOrNull() ?: 20
    }
}