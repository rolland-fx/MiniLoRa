package co.minilora.app.ui.model

import co.minilora.app.data.model.MiniLoRaPayload

enum class DataSourceFlags {
    BLE, TTN, WIFI, BOTH, BLE_WIFI, TTN_WIFI, ALL;

    fun label(): String = when (this) {
        BLE -> "BLE"
        TTN -> "TTN"
        WIFI -> "WiFi"
        BOTH -> "TTN+BLE"
        BLE_WIFI -> "BLE+WiFi"
        TTN_WIFI -> "TTN+WiFi"
        ALL -> "ALL"
    }

    /** Bitmask of the individual feeds this value represents. */
    private val mask: Int
        get() = when (this) {
            BLE -> M_BLE
            TTN -> M_TTN
            WIFI -> M_WIFI
            BOTH -> M_BLE or M_TTN
            BLE_WIFI -> M_BLE or M_WIFI
            TTN_WIFI -> M_TTN or M_WIFI
            ALL -> M_BLE or M_TTN or M_WIFI
        }

    /** Union of two feed sets — the card then shows the combined label. */
    fun combine(other: DataSourceFlags): DataSourceFlags =
        fromMask(mask or other.mask)

    companion object {
        private const val M_BLE = 1
        private const val M_TTN = 2
        private const val M_WIFI = 4

        fun fromMask(m: Int): DataSourceFlags = when (m) {
            M_BLE -> BLE
            M_TTN -> TTN
            M_WIFI -> WIFI
            M_BLE or M_TTN -> BOTH
            M_BLE or M_WIFI -> BLE_WIFI
            M_TTN or M_WIFI -> TTN_WIFI
            else -> ALL
        }
    }
}

data class PayloadHistoryEntry(
    val payload: MiniLoRaPayload,
    val timeUnix: Long,
    val source: DataSourceFlags,
    val deviceKey: String = ""   // identity of the tracker this frame came
                                 // from: BLE MAC, TTN device id or WiFi
                                 // push name; "" = the configured device
)

/**
 * Prune choices for the app's history — mirrors the server's
 * /api/device/{id}/prune windows (cutoff = now − age; "LAST" = newest
 * entry only, like the server's all_but_last).
 */
enum class PruneOption(val label: String) {
    ALL("Delete all"),
    LAST("Keep only the last point"),
    HOUR("Keep the last hour"),
    DAY("Keep the last day"),
    WEEK("Keep the last week"),
    MONTH("Keep the last month"),
}
