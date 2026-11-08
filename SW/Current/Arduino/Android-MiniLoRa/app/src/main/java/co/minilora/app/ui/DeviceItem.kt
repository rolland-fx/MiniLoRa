package co.minilora.app.ui

/**
 * Represents a BLE device discovered during scanning.
 */
data class DeviceItem(
    val name: String,
    val address: String,
    val rssi: Int,
    val discoveredAt: Long,
    val isSelected: Boolean = false
)