# MiniLoRa ProGuard Rules

# Keep MiniLoRaPayload class and its fields (used via reflection by parser)
-keep class co.minilora.app.data.model.MiniLoRaPayload { *; }

# Keep data classes used by RecyclerView adapters
-keep class co.minilora.app.ui.DeviceItem { *; }
-keep class co.minilora.app.ui.model.ScanEntryItem { *; }

# Keep osmdroid classes
-keep class org.osmdroid.** { *; }
