# Flashing, Updating & Device Recovery

Firmware: ESP-IDF **v5.5.1**, target **esp32c6**, 4 MB flash, OTA-capable
dual-slot partition table. The tracker is a *deep-sleep device with NVS
state*, so flashing has one golden rule:

> ⚠️ **Never `erase_flash` a device you care about without taking a full
> flash backup first.** The NVS holds the LoRaWAN keys, WiFi credentials and
> the boot-loop guard state.

## 1. Toolchain

```bash
# Install ESP-IDF v5.5.1 (https://docs.espressif.com/projects/esp-idf/)
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
./esp-idf/install.sh esp32c6
. ./esp-idf/export.sh        # every new shell, before any idf.py command
```

RadioLib and the Zigbee stack come as managed components
(`main/idf_component.yml`: `jgromes/radiolib`, `espressif/esp-zigbee-lib`) —
they are fetched automatically on first build.

> ⚠️ **Version-string trap:** the firmware version lives in `sdkconfig`
> (`CONFIG_MINILORA_FW_VERSION`), which *pins* the Kconfig default once
> generated. Bumping the version in `Kconfig.projbuild` alone does nothing
> for an existing build dir — update `sdkconfig` too (or delete it and
> rebuild). Verify with `grep MINILORA_FW_VERSION build/config/sdkconfig.h`.

## 2. Build & flash

```bash
cd SW/Current/Firmware
idf.py set-target esp32c6        # only if sdkconfig is missing
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

- The device is a **USB-Serial-JTAG** (native USB). 115200 baud.
- ⚠️ **Flash ALL three images** (bootloader + partition table + app) when
  coming from a different partition layout — `idf.py flash` does this
  automatically, but a manual `esptool write-flash 0x20000 minilora.bin`
  alone leaves the device unbootable ("No bootable app partitions").
- ⚠️ **FW 3.0 changed the partition layout** (OTA slots widened to
  `0x1E0000` each by shrinking `zb_storage`/`coredump` to 64 KB): any
  2.6.x device must be flashed over USB **once** (all three images) to
  adopt it — OTA between 2.6 and 3.0 across the old layout will not work.
  Provisioning data in NVS survives the flash; the LoRaWAN session does
  not (one rejoin on the next cycle).
- ⚠️ Opening the port can reset the chip (USB-Serial-JTAG reacts to DTR):
  assert DTR/RTS **low before** `open()` in your capture scripts, and
  expect the port to vanish whenever the device is in deep sleep — it is
  only present during the awake window. Closing the port can also reset
  it (DTR drop on close).
- ⚠️ **Linux hosts: disable ModemManager** (it probes new ACM ports by
  toggling DTR/RTS → endless USB reset loop on the ESP32-C6, device
  re-enumerates every ~60 s, `esp_reset_reason` 11 = `ESP_RST_USB`):
  ```bash
  sudo systemctl stop ModemManager   # immediate
  # permanent, Espressif VID only:
  echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ENV{ID_MM_DEVICE_IGNORE}="1"' \
    | sudo tee /etc/udev/rules.d/99-espressif-jtag.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```
  Symptom check: `journalctl -k | grep ttyACM` — repeated
  "USB disconnect"/"new device" pairs ~60 s apart = a DTR-toggling
  prober (ModemManager/brltty), not a firmware or power problem.
- ⚠️ **Never flash and capture at the same time.** A capture script that
  re-opens the port while esptool holds it makes the write die at a
  random percentage ("No more data to read from the serial port").
  Kill capture tooling before flashing; restart it afterwards.
- ⚠️ **The USB-JTAG DTR line resets and re-bootmodes the chip.** pyserial
  `open()` asserts DTR (→ download mode at the next reset), every
  open/close toggle resets the C6. Capture scripts: `stty -F <port>
  -hupcl` before opening, never poll-reopen faster than 5 s, and set
  dtr/rts low. Symptom of getting it wrong: 15-minute silent USB
  presences (ROM "waiting for download") or reset cascades at
  4/s, all logging `esp_reset_reason` 11 (`ESP_RST_USB`).
- Exit monitor with `Ctrl-]`.

**Build sanity:** the app binary must fit the `0x1E0000` OTA slot (3.0
layout, 1,920 KB); the build prints the headroom (fw 3.0.0 ≈ 1.9 MB,
~5 % free).

## 3. Backup & restore (do this BEFORE touching an existing device)

Full 4 MB image including NVS and Zigbee NVRAM:

```bash
# Backup (device running its old firmware)
esptool.py --port /dev/ttyACM0 --baud 460800 read_flash 0 0x400000 backup.bin
md5sum backup.bin                       # keep the hash!

# Restore
esptool.py --port /dev/ttyACM0 --baud 460800 write_flash 0 backup.bin
```

If `read_flash` complains about the port, the device may be in download mode
— reset it and retry. Verify a restore by reading the flash again and
diffing the two images (must be byte-identical).

## 4. Updating an already-running v2 device

Two paths:

### a) USB (always works)
`idf.py -p /dev/ttyACM0 flash` writes `ota_0`. NVS is **not** touched, so the
config, keys and history survive. If the new image is bad, see the rollback
section — the old slot is still there.

### b) OTA over the air (MiniLoRa-server, FW ≥ 2.5.0)

Upload `build/minilora.bin` on the server's **/ota** page, **Activate** it,
and the device pulls it at its next wake — triggered by:

- the **OTA_UPDATE** remote command (fPort 3 `4D 05 00`; Android app Remote
  tab → *Update firmware*, or the server's notify button via TTN), or
- the periodic check (`MINILORA_OTA_CHECK_EVERY` boots, default 96 ≈ daily),
  which only runs when the WiFi transport is configured and battery is not
  low/critical.

The device SHA-256-verifies the image **before** switching boot slots; a
mismatch or aborted download leaves the running image untouched (and the
rollback machinery of §a applies on top). The check endpoint is
`GET /ota/check?device=<DevEUI>&current=<fw>` — see ICD §9.

> DevEUI is the device identity for OTA: make sure it is set in the config
> (it is, if LoRaWAN is provisioned), or target the image at device_id
> `ANY` on the server.

### Rollback & boot-loop protection (automatic, no user action)

- After flashing a new image it starts in `PENDING_VERIFY`. On the first
  **healthy** boot (config loaded, no recovery mode) the app calls
  `esp_ota_mark_app_valid_cancel_rollback()`.
- If the image crashes before confirming, the bootloader falls back to the
  previous slot — a bad update can never permanently brick a device.
- The **boot-loop guard**: > 3 consecutive unexpected resets latch
  **recovery mode** (minimal cycle, BLE config window, no radio TX) and the
  `recovery` flag rides the diagnostics frame — visible in the app.

## 5. First boot of a virgin device

1. It boots with empty defaults: name `MiniLoRa-FX-1`, LoRaWAN/GNSS/sensors
   enabled, Zigbee/WiFi/beacon off, 15 min sleep. Without keys it will not
   join — that's fine.
2. **Secrets are never baked into the firmware** — provision over BLE
   (long-press ≥ 3 s → Provision tab → write). See [USER_GUIDE.md](USER_GUIDE.md).
3. To verify the cycle, watch the serial log: each wake prints the power
   strategy (`sleeping … ms (bat … mV, low=… critical=…)`) and, on GPS fixes,
   the TTFF.

## 6. Device recovery cheat-sheet

| Situation | Action |
|---|---|
| Lost LoRaWAN keys / misconfigured | long-press → BLE config → rewrite (keys only live in NVS) |
| Bricked by a bad image | power-cycle: bootloader falls back to the other slot; reflash from USB |
| Totally dead / garbage flash | restore the full backup (`write_flash 0 backup.bin`) or erase + flash + re-provision |
| Suspicious power drain | check the diag frame's counters + `powerFlags`; bench-measure with a Joulescope (see `Firmware/docs/POWER_OPTIMIZATION.md`) |
| Want the v1 Arduino firmware back | restore the tag-`Before-Rework` backup taken in `Firmware/docs/HW_CROSSCHECK.md` (md5 `0fd44bbd…`) |

## 7. Host-side tests (no hardware needed)

```bash
cd SW/Current/Firmware/host_tests
cmake --build build && ./build/minilora_tests     # expect: TOTAL: 96 checks, 0 failures
cd ../../TTN && python3 check_vectors.py          # golden-vector checks
```

The firmware, the TTN decoder, the Android app and the test server all share
the same golden vectors (`host_tests/vectors/*.json`, generated by
`Firmware/tools/gen_vectors.py`).
