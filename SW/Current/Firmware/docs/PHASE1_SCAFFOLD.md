# Phase 1 — Project Scaffold

**Status:** ✅ complete — verified by a local `idf.py build` for `esp32c6`
**Decisions & rationale for everything set up in this phase.**

## Layout

```
SW/Current/Firmware/
├── CMakeLists.txt          ESP-IDF project root
├── sdkconfig.defaults      pinned policy (watchdog, OTA, C++, power)
├── partitions.csv          4 MB OTA layout
├── main/
│   ├── Kconfig.projbuild   MINILORA_FW_VERSION, device name defaults
│   └── app_main.cpp        entry point (grows into the state machine)
├── host_tests/             host unit tests + golden vectors (Phase 0/2)
├── tools/gen_vectors.py    golden vector generator (source of truth)
└── docs/                   per-phase notes (this file)
```

## Toolchain

**ESP-IDF v5.5.1**, pinned in CI (`espressif/esp-idf-ci-action@v1`) and used
locally from `~/.espressif/frameworks/esp-idf`. Target `esp32c6` only — no
multi-target cost until the self-made C6 module exists.

## Partition table (4 MB)

| Partition | Size | Why |
|---|---|---|
| `nvs` | 24 KB | config + secrets + boot-loop guard counters |
| `phy_init`, `otadata` | 4+8 KB | standard |
| `ota_0` / `ota_1` | 1.875 MB each | A/B OTA; the full app with Zigbee must fit — verified at every build |
| `coredump` | 128 KB | post-mortem from the field, retrievable over USB/BLE |

No `factory` partition: `ota_0` is the factory (first flash goes to ota_0),
which saves 1.8 MB for app code.

## sdkconfig.defaults — the policy knobs

| Setting | Value | Rationale |
|---|---|---|
| `ESP_TASK_WDT_INIT` + `TIMEOUT_S=180` | on / 180 s | WDT must outlast the longest legit block (GPS window 150 s); feeding is at state-machine phase boundaries, not free-running |
| `BOOTLOADER_APP_ROLLBACK_ENABLE` | on | new image must `esp_ota_mark_app_valid_cancel_rollback()` after a healthy boot cycle, else auto-rollback |
| `COMPILER_CXX_EXCEPTIONS` | on | exceptions used in domain code; panic handler reboots anyway |
| `ESP_SYSTEM_PANIC_PRINT_REBOOT` | on | never hang — reboot into the boot-loop guard |
| `ESP_SLEEP_GPIO_RESET_WORKAROUND` | on | C6 GPIO strapping robustness across deep sleep |

Signing (RSA secure boot) is configured in Phase 6 — it requires burning
eFuses and belongs with the fault-injection work, not the scaffold.

## Versioning

`CONFIG_MINILORA_FW_VERSION` (default `2.0.0-dev`) is baked into the image and
reported via the diagnostics uplink (fport 2, Phase 6) and BLE advertising.
**Rule: no payload/ABI change without a version bump.**

## CI

`.github/workflows/firmware.yml`:
- `idf-build`: builds for esp32c6 on every PR/push touching the firmware — the
  "it must always compile" gate.
- `host-tests`: builds & runs `host_tests` (added in Phase 2).

## Consequences

- The Arduino firmware stays untouched under `SW/Current/Arduino/` as the
  reference implementation and fallback until Phase 4 parity.
- `main/CMakeLists.txt` forces `-std=gnu++2b` for application code; IDF
  components keep their own settings.
