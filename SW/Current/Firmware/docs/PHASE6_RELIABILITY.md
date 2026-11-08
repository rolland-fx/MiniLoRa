# Phase 6 — Reliability & OTA

**Status:** ✅ code complete and building green; fault-injection **execution**
requires hardware (procedures below).

## What was implemented

### 1. Boot-loop guard (app-level)
`DiagStore::onBoot()` (NVS namespace `diag`):
- Expected resets (`ESP_RST_DEEPSLEEP`, `POWERON`, `SW`) → clear the counter.
- Anything else (task WDT, panic, brownout, unknown) → increment.
- `> kMaxUnexpectedResets (3)` → **recovery mode**: BLE config window +
  minimal cycle, no sensors, no radio TX.
- A clean completed cycle clears the counter (`onCleanCycle()`).

### 2. OTA with automatic rollback
- Partition table: `ota_0`/`ota_1`, no factory partition.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: a newly flashed image boots in
  `PENDING_VERIFY` state; `runCycle()` calls
  `esp_ota_mark_app_valid_cancel_rollback()` only **after** NVS config loads
  and the boot-loop guard passes — a firmware that crashes before that point
  is automatically rolled back by the bootloader.
- Update transport: `esp_ota_ops` writes + `esp_https_ota` component are
  already linked; the field-update tool (BLE/serial uploader) is a follow-up
  app — the firmware-side contract is complete.

### 3. Diagnostics uplink (fport 2)
22-byte frame (`domain/DiagPayload.{hpp,cpp}`, host-tested): fw version,
boot count, unexpected resets, last reset reason, and saturating failure
counters per transport. Sent every `kDiagSendEveryCycles = 16` boots
(≈1/day at default intervals) so main telemetry is never perturbed.
fport-2 frames skip downlink processing by design.

### 4. Watchdog (from Phase 1, finalized here)
- Task WDT 180 s, subscribed by the app task at cycle start.
- Fed at phase boundaries + GPS retry loop; timeout > GPS window (150 s).
- Brownout detector on (default).
- `ESP_SYSTEM_PANIC_PRINT_REBOOT`: panics reboot into the boot-loop guard
  rather than hanging.

### 5. Image signing — deliberately deferred
`CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME` requires generating an RSA key and is
irreversible per-device once secure boot is burned. Decision: **enable it on
the bench before field deployment**, using
`esp-idf: secure boot v2 guide` (key generation → `sdkconfig` → one-time
eFuse burn). Documented here so it isn't forgotten; rollback protection
already works without it.

## Fault-injection test matrix (on hardware)

| Test | Procedure | Expected |
|---|---|---|
| Hung task | Temporarily `while(1);` in a phase | Task WDT reset after 180 s |
| Boot loop | `abort()` in `app_main` | 3 unexpected resets → recovery mode (BLE window, no TX) |
| Bad image | Flash truncated/old binary to ota_1 + set otadata | Bootloader rolls back to ota_0 |
| Panic | `ESP_ERROR_CHECK` on a failing call | Reboot, counter increments, fport-2 shows reason |
| Brownout | Set brownout threshold high, force LoRa TX | Reset, counter increments |
| NVS corruption | Truncate `config` namespace | Defaults loaded, device configurable via BLE |

## Host-test coverage for this phase

`test_diag.cpp` — fport-2 layout: version bytes, LE counters, saturation at
65535, reserved bytes zero. Total suite: 85 checks green.
