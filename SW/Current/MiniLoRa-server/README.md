# MiniLoRa-server

The server side of MiniLoRa: receives the tracker's WiFi push, pulls data
from **The Things Network** (webhook and/or storage-API poller), stores
everything in SQLite and serves a **web UI with graphs, a map and a
diagnostics dashboard**. Replaces the old Wifi-Test-Server (now in `archive/v1-wifi-test-server/`).

```
tracker ──POST /data (28 B)──▶┐
                              │   MiniLoRa-server
phone app ◀──JSON API─────────┤   FastAPI · SQLite · Chart.js/Leaflet
TTN webhook ──POST───────────▶│   config.toml
TTN storage API ◀──poll───────┘
```

## Quick start (Docker)

```bash
cd SW/Current/MiniLoRa-server
cp config.example.toml config.toml     # edit: TTN credentials, device_groups…
docker compose up -d --build
# → http://localhost:8080
```

Data lives in `./data/minilora.sqlite3` (bind-mounted), config in
`./config.toml`. The container **always serves on 8080** (bind and port come
from the image's environment, not the mounted config — a `config.toml`
written for a native run on :5000 cannot break the mapping). To update:
`git pull && docker compose up -d --build` — the SQLite file survives.

## Deployment notes (LAN, set-and-forget)

Two guarantees keep a long-running install boring:

- **Logs never grow.** Set `log_file` in `[server]` (or `MINILORA_LOG_FILE`
  — the Docker image defaults it to `/data/minilora.log`, on the mounted
  volume) and rotation keeps on-disk logs at
  `log_max_mb × (1 + log_backups)` — default 5 MB × 4 = ~20 MB, forever.
- **The database never exceeds its cap.** The `[retention]` section enforces
  per-device newest-N limits and a global ceiling (`db_max_mb`, default
  100 MB) hourly, oldest-first. Measured cost: **~200 B per reading**
  (indexes + diagnostics share included) → **10,000 records ≈ 2 MB** and
  **100 MB ≈ ~500,000 readings** — years of multi-tracker data at the
  15-min cycle. Benchmarked at that scale: page queries 7 ms, analytics
  aggregates 180–270 ms, the hourly retention pass ~60 ms, VACUUM 131 ms.
  `GET /api/retention` shows the config + current size.

```toml
[server]
log_file = "minilora-server.log"   # empty = stderr only
log_max_mb = 5
log_backups = 3

[retention]
enabled = true
readings_per_device = 200_000      # ~40 MB per device ceiling
db_max_mb = 100
```

## Production checklist (LAN-only)

- **One writer per data directory.** Run *either* the Docker stack *or* a
  native instance on the same `data/` — never both (two pollers = double
  pulls; two retention loops fighting). Migrating: stop the native
  instance, then `docker compose up -d --build` on the same directory.
- **Never port-forward the container to the internet.** The API is
  LAN-trusted by design: `push_token` protects the write endpoints, but the
  tracker's WiFi push has **no token support in the firmware yet** —
  enabling it would drop every device push. (Adding `?token=` to
  `WifiTransport` is a one-line firmware change if you ever need it.)
- **Health**: the image ships a `HEALTHCHECK` on `/health` — `docker ps`
  shows healthy/unhealthy and `restart: unless-stopped` recovers crashes.
- **Both log streams are capped**: the app's rotating file (see above) and
  the container console (`json-file`, 3 × 5 MB in compose).
- **Timezone**: set `TZ` in compose (default Europe/Paris) — the UI renders
  times in the container's timezone.
- **Backups** — cron the consistent snapshot (the SQLite backup API, safe
  while running) and include the OTA firmware directory:
  ```cron
  17 3 * * * curl -sf http://localhost:8080/api/export/db -o /backup/minilora-$(date +\%u).sqlite3
  ```
  (`/backup/minilora-1..7.sqlite3` = 7 rotating copies; `cp -r data/firmware`
  monthly covers the OTA binaries.)
- **Resources**: `mem_limit: 256m` in compose is generous (idle ≈ 30 MB).
- **Ownership**: compose runs the container as `user: "1000:1000"` so
  `./data` stays user-owned — match your `id -u` if it differs.
- **Updates**: `git pull && docker compose up -d --build` — data, logs and
  the firmware directory survive; the DB cap and rotation make the image
  the only thing that ever changes size.

## Proxmox VE (LXC, no Docker)

For a dedicated VM-host deployment: `deploy/proxmox-lxc.sh` creates an
**unprivileged Debian 12 LXC** running the server natively (python venv +
systemd — no Docker nesting, no USB passthrough needed, OTA-only).

```bash
# on the Proxmox host, from a clone of this repo (deploy/ dir):
bash deploy/proxmox-lxc.sh                        # next free CT id, DHCP
bash deploy/proxmox-lxc.sh --id 120 --ip 192.168.1.50/24 --gateway 192.168.1.1
bash deploy/proxmox-lxc.sh --repo https://github.com/you/minilora --branch main
bash deploy/proxmox-lxc.sh --update 120           # re-sync code + restart
```

512 MB RAM / 1 core / 4 GB disk. Data in `/opt/minilora-server/data`
(bounded logs + the 100 MB retention apply), config written once at
`/opt/minilora-server/config.toml` and never touched by updates.
Backups: PVE vzdump snapshots the whole container; the
`/api/export/db` cron works the same. The container logic is exercised
end-to-end in a Debian 12 test (install → idempotent re-run → boot →
health/retention) — the `pct` commands themselves are the standard ones.

## Local run (no Docker)

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
python -m minilora_server.main          # reads config.toml, default 0.0.0.0:8080
```

Tests (validate the decoders against the shared golden vectors):

```bash
pip install pytest
python -m pytest tests/ -q
```

## Configuration (`config.toml`)

| Section | Key | Meaning |
|---|---|---|
| `[server]` | `host`, `port` | bind address (default 0.0.0.0:8080; **Docker overrides both** via env) |
| | `db_path` | SQLite file (Docker: `/data/…` via env) |
| | `push_token` | shared secret for write endpoints (empty = open) |
| `[wifi]` | `default_device` | label for pushes without `?device=` |
| `[ttn]` | `webhook_token` | token checked on `/api/ttn/webhook` |
| | `enabled` | **turn on the automatic 12 h storage-API poller** (needs the keys below) |
| | `region`, `app_id`, `api_key`, `device_ids` | storage-API poller credentials (same endpoint/parameters as the Android app) |
| | `poll_interval_s`, `poll_limit`, `poll_window_h` | loop every 5 min, newest 20 frames per request, re-fetch 12 h (free tier keeps 24 h — set `poll_window_h` < 24) |
| `[device_groups]` | `Name = ["id1", "id2"]` | dashboard grouping: a tracker reporting under several identities (WiFi push name + TTN id) renders as ONE row with transport chips. **The group label is itself a valid device key** on every `/device/…` page and read API — it opens a merged view of all members (overview, charts, data, diagnostics, analytics, exports); writes (commands, prune, downlink-clear) resolve to the TTN member. Deletes stay per-identity |

Env overrides (used by Docker): `MINILORA_CONFIG`, `MINILORA_DB`,
`MINILORA_HOST`, `MINILORA_PORT`.

## Ingest paths

1. **Device WiFi push** — the tracker's `WifiTransport` POSTs the 28-byte
   payload v1 frame to `http://IP:PORT/data?device=NAME`
   (`application/octet-stream`). The name comes from the device config
   (v2 firmware always sends it); a push without `?device=` falls back to
   the client IP (v1 server compatibility). Key history by the name, not
   the IP — DHCP re-addressing must not orphan data.
   **Diagnostics ride the same radio**: when a diag frame is due (same
   ~1/day cadence or a remote SEND_DIAG), the device also POSTs the
   fPort-2 frame (24/28/29 B) to `/diag?device=NAME` — the only diag source
   when LoRaWAN is down or disabled. Deliberately a separate endpoint: a
   diag frame decoded as telemetry would read as a bogus 3–4 °C
   temperature. 60-s retry dedup on the diagnostics table.
2. **TTN poller (automatic)** — with `[ttn] enabled = true` the server
   runs a background loop that re-fetches the last **12 h** of uplinks
   every `poll_interval_s` (default 5 min) and stores anything new. Free
   TTN accounts only keep 24 h of storage — this keeps the last 12 h
   permanently hot, so nothing is ever lost even if the server is down
   for hours. **Dedup is exact**: each uplink is keyed by its TTN
   `received_at` timestamp (telemetry/diagnostics) or by the payload's
   own fix epoch (fPort-4 history entries), so re-polling the same
   window never creates duplicates. Fill `app_id`, `api_key`
   (storage-read link rights suffice), `device_ids` in `config.toml`.
3. **TTN webhook** — real-time alternative: TTN → `POST /api/ttn/webhook?token=…`
   (needs a public URL or LAN reachability).

All three feed the same decoders (validated against
`Firmware/host_tests/vectors/*.json` — payload v1, diag 0x03/0x04/0x05,
history 0x01). fPort-4 **position-history** frames are expanded into one
reading per entry with source `ttn-hist`, so underground periods still plot
on the map and charts.

## Web UI

| Page | Content |
|---|---|
| `/` | devices (**grouped per physical tracker** via `[device_groups]`, transport chips per identity) + API reference. The group label and *open* link to the **merged view**: every member identity's data mixed into one timeline |
| `/device/{id}` | **Overview** — the latest-reading hero cards (with their date/time) + the position map (latest fix + trail). `{id}` is a device id **or a group label** (label → all members merged) |
| `/device/{id}/charts` | the six timeseries: temp/hum/pressure/battery/lux **+ accelerometer X/Y/Z (g)** — merged across the group when `{id}` is a label; a **1 h…48 h/All window selector** clamps the time axis in place |
| `/device/{id}/remote` | **Remote control** — the five fPort-3 commands, the ICD 3.5 remote-config toggles (WiFi/Zigbee/BLE/GNSS/BME280/OPT3001/LIS3DH), the ICD 3.6 sleep-duration selector and a **Clear TTN downlink queue** button |
| `/device/{id}/data` | maintenance (TTN fetch-now, prune windows, delete) + the readings table — on a group page the `src` column shows each row's transport and deletes stay per-identity |
| `/device/{id}/diagnostics` | diag-frame history: boots, unexpected resets, every failure counter, power flags (`LOW BAT`/`CRIT`), OTA-pending, recovery mode, `config_flags` |
| `/device/{id}/analytics` | **range picker (7/30/90/365 d/all)**, summary cards (avg/min/max temp, humidity, pressure, battery slope mV/day, GPS distance, radio-on time), per-day min/avg/max charts, coverage bars — all backed by SQLite aggregates |
| `/config-builder` | **device `.cfg` file generator** — fill the form (identity, WiFi, transports, sleep, asset mode), download the native 168-byte ICD §4 envelope, import it in the Android app (Provision → Import .cfg) and write it over BLE. The file contains keys in clear text — treat it like a credential |
| `/api/docs` | interactive OpenAPI for all endpoints |

Commands queued here are delivered over TTN exactly like the app's
(POST `/api/device/{id}/command`, ICD §3/3.5/3.6) — the device page's
*Remote* tab and the phone's Remote tab are two views of the same pipe.

The generated envelope is byte-identical to what the app's BLE write
sends (`'C' | 4 | 164-byte body | CRC16-CCITT LE`) — validated against
the app's `BLEDeviceConfigurator.buildBody()` layout.

## Data export

| Endpoint | Content |
|---|---|
| `GET /api/export/{id}.csv?since&until` | all readings as CSV (epoch + raw row, 1:1 with the DB table) |
| `GET /api/export/{id}.json?since&until` | same rows as JSON (scripting) |
| `GET /api/export/{id}/diagnostics.csv` | diag-frame history as CSV |
| `GET /api/export/db` | **consistent SQLite snapshot of everything** (readings, diagnostics, devices, firmware) |
| `GET /api/analytics/{id}?days=N` | summary + daily aggregates as JSON (what the analytics page renders) |

`since`/`until` are unix epochs. The analytics math lives in
`minilora_server/analytics.py` (unit-tested: sentinels like hum=255 and
garbage pressure are excluded, battery slope is mV/day, GPS distance is
a haversine track sum).

Charts and tiles are CDN-loaded (Chart.js, Leaflet) — no build step.

## HTTP API summary

```
POST /data?device=NAME[&token=]     28-byte octet-stream push (device)
POST /diag?device=NAME[&token=]     diag frame push (24/28/29 B — device)
POST /api/ttn/webhook?token=        TTN v3 event (JSON)
GET  /api/devices                   device list + last seen
GET  /api/readings/{id}?limit&since decoded readings (newest first)
GET  /api/latest/{id}               single latest row (raw_hex included)
GET  /api/recent/{id}?limit=N      last N readings (≤500) — the app's
                                    backfill: deep enough to reconstruct
                                    the timeline after days of app-closed
                                    silence, so its graphs never gap
GET  /api/diagnostics/{id}          diag frames (raw_hex included)
GET  /api/analytics/{id}?days=N     summary + daily aggregates (JSON)
GET  /api/export/{id}.csv           readings CSV (also .json, /diagnostics.csv)
GET  /api/export/db                 consistent SQLite backup
GET  /api/stats                     counts
POST /api/device/{id}/command       queue a remote command via TTN downlink
                                    (cmd=locate_now|send_diag|reset_diag|
                                    ble_window|ota_update|config_set&flag&on|
                                    set_range&index — ICD §3/3.5/3.6)
POST /api/device/{id}/downlink/clear
                                    cancel ALL pending downlinks (TTN-wide)
POST /api/device/{id}/prune?older_than=1h|1d|1w|30d|all_but_last
                                    delete readings/diagnostics older than
                                    the window (all_but_last: keep only the
                                    newest reading + diagnostic)
DELETE /api/device/{id}             remove device + ALL its data (no undo)
GET  /health                        liveness
```

Data maintenance is also on the web UI: per-device *delete older than*
buttons on the device page and a *delete* button per row on the dashboard —
both ask for confirmation. Deletion is permanent; take a backup first
(`/api/export/db`) if the history matters. If a deleted device reports
again it reappears as a fresh entry.

## Device-side note (v2 firmware)

`WifiTransport` posts the payload to `http://IP:PORT/data` — firmware
**≥ 2.3.1** actually writes the body (earlier builds posted an empty
body; fixed alongside this server). Set the server IP/port in the app's
Config tab; the port must match `[server] port` (default 8080, not the old
5000).

## Home Assistant integration (MQTT discovery)

The server can publish every device to **any** Home Assistant instance via
MQTT discovery — HA auto-creates the entities, no HA-side config beyond its
existing MQTT integration. Requires an MQTT broker (e.g. the **Mosquitto
add-on** on your HA box).

```toml
[ha]
enabled = true
broker = "192.168.1.x"   # host running Mosquitto
port = 1883
username = "minilora"    # broker user (empty = anonymous)
password = "..."
```

Restart the server: a **device card** appears in HA (*Settings → Devices &
Services → MQTT*) per tracker, grouped so each device keeps all its entities
together. **On the wire** (smoke test without HA):

```
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'minilora/#' -v
mosquitto_sub -h <broker> -t 'homeassistant/+/minilora/+/config' -v
```

Entities per device:

| Entity | Type | Notes |
|---|---|---|
| **Position** | `device_tracker` | HA map + position history; telemetry rides along as attributes |
| Temperature / Humidity / **Pressure** | sensors | pressure in true hPa (ICD 3.3 deci-hPa chain) |
| Battery (V) / Illuminance / Last update | sensors | `state_class: measurement` → HA long-term statistics |
| Altitude, satellites, accel X/Y/Z, awake, sleep | sensors | **disabled by default** — enable in the entity settings when wanted |
| Diagnostics (boots, resets, GPS timeouts, join/WiFi failures, TTFF, link margin, gateways) | sensors | on their own `diag` topic, **disabled by default** |

Design notes: state rides **one JSON topic per device**
(`minilora/device/<id>/state`) with per-entity `value_template`s — atomic
updates, no topic sprawl; GPS positions go to a dedicated `…/gps` topic; a
retained **last-will** (`minilora/server/status`) marks all entities
*unavailable* if the server dies; discovery configs are **retained** and
re-announced on every (re)connect, so broker/HA restarts self-heal. History
rows (fPort-4) only move the tracker, never the sensor values. New devices
appear automatically on their first uplink.

## OTA firmware hosting (device-pull)

The `/ota` page manages firmware images: **upload** `build/minilora.bin`,
**Activate** (per device target, or ANY), and optionally **Notify** a device
via a TTN OTA_UPDATE downlink (needs `[ttn]` credentials). **Targeting gotcha:**
the device identifies itself by **DevEUI** in its check
(`?device=70B3D5…`), so a per-device target must be the **DevEUI**, not the
TTN device id — a mistargeted image silently offers no update (verified
2026-11-02). The device page's device id and the DevEUI only coincide by
luck. The device pulls on its next wake:

```
GET /ota/check?device=<DevEUI hex>&current=<fw>   -> JSON (see ICD §9)
GET /ota/download/{id}                             -> the .bin
```

Integrity: the server computes SHA-256 at upload; the device verifies the
digest **before** switching boot slots. Images are stored under
`data/firmware/`, metadata in the `firmware` table. The periodic check runs
every `MINILORA_OTA_CHECK_EVERY` boots (firmware Kconfig, default 96) when
WiFi is configured and battery is healthy.

## Data model

- `readings` — one row per telemetry frame (decoded fields + `raw_hex`)
- `diagnostics` — one row per diag frame
- `devices` — `first_seen` / `last_seen` / `last_source` per device

Duplicates (same `raw_hex` from the poller/webhook within 60 s) are dropped
so TTN back-fill windows don't double-store.

## Screenshots

> **TODO — screenshots to add.** Drop the PNGs into `docs/screenshots/`
> (repo root) and replace the file names with real `![...](...)` embeds.

| Page | File to add |
|---|---|
| Dashboard (grouped devices + transport chips) | `server-dashboard.png` |
| Device Overview (hero cards + position map) | `server-overview.png` |
| Charts (six timeseries + window selector) | `server-charts.png` |
| Remote (commands, remote config, clear queue) | `server-remote.png` |
| Data (maintenance + readings table) | `server-data.png` |
| Diagnostics (counters, flags, config_flags) | `server-diagnostics.png` |
| Analytics (summary cards + per-day charts) | `server-analytics.png` |
