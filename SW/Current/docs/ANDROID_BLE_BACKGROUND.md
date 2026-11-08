# Android: Background BLE Telemetry — Implementation Plan

Status: **planned, not implemented** (written 2026-11-01, after bench validation of LoRaWAN join).
Scope: `Arduino/Android-MiniLoRa` app only. **Firmware needs zero changes** — the device already
broadcasts the full 28-byte payload in manufacturer data of a BLE 5 extended advertisement
(coded PHY, ~1 Hz while awake, see `Arduino/Android-MiniLoRa/BLE_BEACON.md`).

## Current state (why it doesn't work in background today)

- `BleScanner` (`ble/BleScanner.kt`) is a plain class owned by `AppViewModel`; scan runs only
  while the Tracker tab is open — `MainActivity.onPause()` → `stopScanOnPause()` → `scanner.stop()`.
- Two simultaneous scans (both with **empty ScanFilter lists**): legacy-PHY scan for the
  config-window advertisement, extended/coded-PHY scan for telemetry beacons.
- GATT (`BLEDeviceConfigurator.kt`) is connect-op-disconnect on demand; unaffected by this plan.
- TTN polling (`TTNClient`) and WiFi polling loops run in `viewModelScope` — they die when the
  app is backgrounded or killed.
- No `Service`, no `WorkManager`, no `NotificationChannel` anywhere. `POST_NOTIFICATIONS` is
  declared in the manifest but never requested/used.
- `minSdk 33`, `targetSdk 36` — no legacy permission branches needed.

## Hard Android constraints that shape the design

1. **Foreground Service is the only reliable way to scan with the screen off** on modern Android.
2. **Unfiltered scans return nothing with the screen off** (Android 8.1+ rule). The current
   unfiltered scans *must* become filtered, or background mode receives no beacons at all.
   Filter on `setDeviceName("MiniLoRa-FX-1")` and/or `setDeviceAddress(<target MAC from
   Settings>)` — the user's stored target device is the natural filter.
3. `foregroundServiceType="connectedDevice"` is mandatory for targetSdk 34+, plus the
   `FOREGROUND_SERVICE_CONNECTED_DEVICE` and `FOREGROUND_SERVICE` manifest permissions.
4. A foreground service must show a notification → runtime `POST_NOTIFICATIONS` request needed.
5. Filtered scans also massively reduce battery drain vs. the current unfiltered ones.

## Phase 1 — Foreground service skeleton (~1–2 h)

- [ ] Manifest: add both `FOREGROUND_SERVICE*` permissions and the `<service>` entry with
      `android:foregroundServiceType="connectedDevice"`.
- [ ] `BleTelemetryService` (START_STICKY): creates a low-priority notification channel
      ("Telemetry"), promotes itself to foreground, keeps running until told to stop.
- [ ] Request `POST_NOTIFICATIONS` (and confirm `BLUETOOTH_SCAN`/`CONNECT`/fine location are
      still granted) before starting the service.
- [ ] Settings screen: "Background telemetry" toggle → `startForegroundService()` / `stopService()`.

## Phase 2 — Scan filters + repository refactor (~half a day)

- [ ] New tiny singleton `TelemetryBus` (or similar): `MutableStateFlow<PayloadHistoryEntry>`
      + scan-state flows. `MiniLoRaPayloadParser` is pure/static and reused as-is.
- [ ] `BleScanner` writes into the bus instead of being owned by `AppViewModel`;
      `AppViewModel` collects from the bus. Remove `stopScanOnPause()`.
- [ ] Add `ScanFilter`s to both scans (device name and/or stored target MAC). Config-window
      scan keeps legacy PHY, telemetry scan keeps coded PHY — filters don't change the PHY.
- [ ] Service owns scanner start/stop; updates the persistent notification with the latest
      reading, **throttled to ~1/min** (not once per beacon).
- [ ] Handle `onTaskRemoved`: keep running (that's the point) but consider a "stop" action
      button on the notification so the user can kill it deliberately.

## Phase 3 — (optional) move TTN/WiFi polling into the service

- [ ] The `while(true) { fetch; delay }` loops in `AppViewModel` (`toggleTTNPolling`, WiFi
      poll) die on backgrounding. Migrate them into the same foreground service if background
      telemetry should include TTN/WiFi sources, not just BLE.
- [ ] Do **not** use WorkManager for BLE (15-min minimum period can miss the device's awake
      window entirely); WorkManager is acceptable as a TTN catch-up when the service is off.

## Phase 4 — Validation on the real phone (the fiddly part)

- [ ] Screen off → beacons still arrive (this is where OEM scan throttling shows up; a Pixel
      is fine, aggressive OEMs may not be).
- [ ] Doze mode (device stationary + unplugged) → service still receives beacons.
- [ ] Swipe app from recents → service survives (START_STICKY) or restarts.
- [ ] Battery: continuous coded-PHY scanning drains the phone — use `SCAN_MODE_BALANCED`
      (1 Hz beacons don't need LOW_LATENCY) and set expectations: updates arrive as a burst
      each device wake cycle (default config: every 15 min), not continuously.

## Risk / de-risk first step

Before the full refactor: add the scan filters and run the foreground service with the
*existing* scan callbacks. One evening of testing tells us whether coded-PHY extended
beacons are received reliably with the screen off on the actual phone. If not, the whole
feature needs rethinking (e.g. fall back to legacy-PHY beacon or TTN-poll-only background).

## Effort summary

| Phase | Effort |
|---|---|
| 1. FGS skeleton + manifest + notification | 1–2 h |
| 2. Filters + repository + Settings toggle | ~0.5 day |
| 3. TTN/WiFi polling migration (optional) | ~0.5 day |
| 4. Real-device validation | fiddly, plan an evening |

Total for a solid phase 1+2: **roughly one day**.
