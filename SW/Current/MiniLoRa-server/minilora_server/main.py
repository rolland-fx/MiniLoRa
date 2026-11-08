"""MiniLoRa-server — FastAPI application.

Receives the tracker's WiFi push (POST /data, 28-byte octet-stream — the
device's native protocol), accepts TTN webhooks, optionally polls the TTN
storage API, stores everything in SQLite and serves a graphed web UI.
"""

import asyncio
import hashlib
import logging
import re
import time
from datetime import datetime
from pathlib import Path
from typing import Optional

import uvicorn
from fastapi import (FastAPI, File, Form, HTTPException, Query, Request,
                     UploadFile)
from fastapi.responses import (HTMLResponse, JSONResponse, PlainTextResponse,
                               RedirectResponse, Response)
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates

from .config import AppConfig, load_config
from .decode import decode_diagnostics, decode_telemetry
from .cfgbuild import SLEEP_MINUTES
from .db import Database
from .ttn import TTNPoller, decode_frame, parse_uplink_event

log = logging.getLogger("minilora")

BASE = Path(__file__).resolve().parent.parent
TEMPLATES = Jinja2Templates(directory=str(BASE / "templates"))

cfg: AppConfig = load_config(str(BASE / "config.toml"))
db = Database(cfg.server.db_path)


def check_push_token(request: Request, token: Optional[str]) -> None:
    """Shared secret for write endpoints (empty in config = open)."""
    expected = cfg.server.push_token or cfg.ttn.webhook_token
    if not expected:
        return
    given = token or request.headers.get("X-MiniLoRa-Token", "")
    if given != expected:
        raise HTTPException(status_code=403, detail="invalid token")


app = FastAPI(title="MiniLoRa-server", version="1.0.0",
              docs_url="/api/docs", openapi_url="/api/openapi.json")
app.mount("/static", StaticFiles(directory=str(BASE / "static")), name="static")


# ------------------------- ingest: device WiFi push -------------------------

@app.post("/data")  # v1 device ABI (WifiTransport posts 28 B octet-stream here)
@app.post("/api/telemetry")
async def receive_data(request: Request,
                       device: str = Query(None),
                       token: str = Query(None)):
    check_push_token(request, token)
    body = await request.body()
    # Single frame (v1 ABI) or a bulk flush of n concatenated 28-byte
    # frames (device store-and-forward buffer drained over WiFi).
    if len(body) < 28 or len(body) % 28 != 0:
        raise HTTPException(400,
                            f"expected 28 bytes or a multiple of 28, got {len(body)}")
    device_id = device or request.client.host or cfg.wifi.default_device
    stored = 0
    for off in range(0, len(body), 28):
        frame = body[off:off + 28]
        try:
            decoded = decode_telemetry(frame)
        except ValueError as e:
            # Skip, don't abort: earlier frames of the batch are already
            # stored and the device drops them after a 200 — aborting here
            # would re-send the whole buffer forever.
            log.warning("bulk frame at offset %d undecodable (%s)", off, e)
            continue
        if db.has_duplicate(device_id, "wifi", frame.hex()):
            continue
        db.insert_reading(device_id, "wifi", decoded, frame.hex())
        stored += 1
    if len(body) > 28:
        log.info("wifi bulk push from %s: %d/%d frames stored",
                 device_id, stored, len(body) // 28)
    else:
        log.info("wifi push from %s: %.1f°C %d%% %.0f hPa %d mV",
                 device_id, decoded["temp_c"], decoded["hum_pct"],
                 decoded["pressure_hpa"], decoded["bat_mv"])
    return {"ok": True, "device": device_id, "stored": stored}


@app.post("/diag")  # device WiFi phase pushes the fPort-2 diag frame here
@app.post("/api/diagnostics")
async def receive_diag(request: Request,
                       device: str = Query(None),
                       token: str = Query(None)):
    """Diagnostics over WiFi: same 24-byte (schema 0x03) / 28-byte (0x04) /
    29-byte (0x05) frame the device sends on LoRaWAN fPort 2, POSTed as
    octet-stream. Kept separate from /data on purpose — a diag frame decoded
    as telemetry would read as a bogus 3-4 °C temperature (schema byte in
    byte 0)."""
    check_push_token(request, token)
    body = await request.body()
    if len(body) not in (24, 28, 29):
        raise HTTPException(400, f"expected 24, 28 or 29 bytes, got {len(body)}")
    try:
        decoded = decode_diagnostics(body)
    except ValueError as e:
        raise HTTPException(400, str(e))
    device_id = device or request.client.host or cfg.wifi.default_device
    if db.has_diag_duplicate(device_id, body.hex()):
        return {"ok": True, "duplicate": True}
    db.insert_diagnostic(device_id, decoded, body.hex(), source="wifi-diag")
    log.info("wifi diag push from %s: fw %s, boots %d, resets %d",
             device_id, decoded.get("fw_version"),
             decoded.get("boot_count", 0), decoded.get("unexpected_resets", 0))
    return {"ok": True, "device": device_id}


# ------------------------- ingest: TTN webhook -------------------------

@app.post("/api/ttn/webhook")
async def ttn_webhook(request: Request, token: str = Query(None)):
    check_push_token(request, token)
    event = await request.json()
    stored = 0
    for up in parse_uplink_event(event):
        try:
            kind, decoded = decode_frame(up["f_port"], up["bytes"])
        except ValueError as e:
            log.info("webhook frame skipped (%s): %s", up["device_id"], e)
            continue
        raw_hex = up["bytes"].hex()
        if kind == "telemetry":
            if db.has_duplicate(up["device_id"], "ttn", raw_hex):
                continue
            db.insert_reading(up["device_id"], "ttn", decoded, raw_hex)
        elif kind == "history":
            for entry in decoded["entries"]:
                if db.has_duplicate(up["device_id"], "ttn-hist",
                                    raw_hex + str(entry["gps_time"])):
                    continue
                db.insert_reading(up["device_id"], "ttn-hist",
                                  _history_as_reading(entry), raw_hex)
        else:
            db.insert_diagnostic(up["device_id"], decoded, raw_hex)
        stored += 1
    return {"ok": True, "stored": stored}


# ------------------------- read APIs -------------------------

@app.get("/api/devices")
async def api_devices():
    return [dict(r) for r in db.devices()]


@app.get("/api/readings/{device_id}")
async def api_readings(device_id: str, limit: int = Query(500, le=5000),
                       since: int = Query(None)):
    return db.readings(device_id, limit=limit, since=since)


@app.get("/api/latest/{device_id}")
async def api_latest(device_id: str):
    rows = db.readings(device_id, limit=1)
    if not rows:
        raise HTTPException(404, "no data for device")
    return rows[0]


@app.get("/api/recent/{device_id}")
async def api_recent(device_id: str, limit: int = Query(10, ge=1, le=500)):
    """Last N readings (newest first) for the Android app's history
    backfill — each row carries raw_hex so the app reuses its byte-exact
    payload parser (same contract as /api/latest)."""
    rows = db.readings(device_id, limit=limit)
    if not rows:
        raise HTTPException(404, "no data for device")
    return [dict(r) for r in rows]


@app.get("/api/diagnostics/{device_id}")
async def api_diagnostics(device_id: str):
    return db.diagnostics(device_id)


@app.get("/api/stats")
async def api_stats():
    return db.stats()


@app.get("/health", response_class=PlainTextResponse)
async def health():
    return "ok"


@app.get("/api/retention")
async def api_retention():
    """Retention config + current DB size — the deployment dashboard for
    the 'DB never exceeds its cap' promise."""
    return {
        "enabled": cfg.retention.enabled,
        "readings_per_device": cfg.retention.readings_per_device,
        "diagnostics_per_device": cfg.retention.diagnostics_per_device,
        "db_max_mb": cfg.retention.db_max_mb,
        "db_size_mb": round(db.size_bytes() / 1e6, 2),
    }


# ------------------------- config builder -------------------------

@app.get("/config-builder", response_class=HTMLResponse)
async def ui_config_builder(request: Request):
    return TEMPLATES.TemplateResponse(request, "config_builder.html",
                                      {"error": None, "form": {},
                                       "stats": db.stats()})


def _cfg_from_form(form) -> tuple[bytes, str]:
    """Validate the builder form -> (envelope bytes, filename)."""
    from .cfgbuild import SLEEP_MINUTES, build_envelope

    def s(key: str, default: str = "") -> str:
        return str(form.get(key, default) or "").strip()

    def flag(key: str) -> bool:
        return form.get(key) in ("on", "true", "1")

    def hexint(key: str, digits: int) -> int:
        raw = s(key).replace(" ", "").lower()
        if not raw:
            return 0
        if len(raw) > digits or any(c not in "0123456789abcdef" for c in raw):
            raise ValueError(f"{key}: expected up to {digits} hex digits")
        return int(raw, 16)

    def hexbytes(key: str, nbytes: int) -> bytes:
        raw = s(key).replace(" ", "").lower()
        if not raw:
            return bytes(nbytes)
        if len(raw) != nbytes * 2 or any(c not in "0123456789abcdef" for c in raw):
            raise ValueError(f"{key}: expected exactly {nbytes * 2} hex digits")
        return bytes.fromhex(raw)

    name = s("name") or "MiniLoRa"
    if len(name) > 32:
        raise ValueError("name: max 32 characters")
    ssid, password = s("ssid"), s("password")
    if len(ssid) > 32 or len(password) > 32:
        raise ValueError("wifi ssid/password: max 32 characters each")

    ip = s("ip")
    octets = ip.split(".")
    if len(octets) != 4 or not all(
            o.isdigit() and 0 <= int(o) <= 255 for o in octets):
        raise ValueError("ip: expected dotted quad (e.g. 192.168.1.10)")

    try:
        port = int(s("port", "5000"))
    except ValueError:
        raise ValueError("port: not a number")
    if not 1 <= port <= 65535:
        raise ValueError("port: out of range 1-65535")

    rng = int(s("range", "4"))
    if not 0 <= rng <= 8:
        raise ValueError("range: 0-8")
    motion = int(s("motionSens", "2"))
    if not 0 <= motion <= 3:
        raise ValueError("motionSens: 0-3")

    def byte(key: str, default: int) -> int:
        try:
            v = int(s(key, str(default)))
        except ValueError:
            raise ValueError(f"{key}: not a number")
        if not 0 <= v <= 255:
            raise ValueError(f"{key}: 0-255")
        return v

    env = build_envelope(
        name=name, ssid=ssid, password=password, ip=ip, port=port,
        join_eui=hexint("joinEUI", 16), dev_eui=hexint("devEUI", 16),
        app_key=hexbytes("appKey", 16), nwk_key=hexbytes("nwkKey", 16),
        cfg_lorawan=flag("cfgLoRaWAN"), cfg_wifi=flag("cfgWiFi"),
        cfg_gnss=flag("cfgGNSS"), cfg_bme280=flag("cfgBME280"),
        cfg_opt30=flag("cfgOPT30"), cfg_lis3dh=flag("cfgLIS3DH"),
        cfg_ble=flag("cfgBLE"), cfg_zigbee=flag("cfgZigbee"),
        cfg_range=rng, cfg_asset_mode=flag("cfgAssetMode"),
        cfg_motion_sens=motion,
        cfg_asset_heartbeat_h=byte("assetHeartbeatH", 24),
        cfg_burst_interval_min=byte("burstIntervalMin", 5),
        cfg_burst_max_per_day=byte("burstMaxPerDay", 20),
    )
    fname = "minilora-" + re.sub(r"[^A-Za-z0-9_-]+", "_", name) + ".cfg"
    return env, fname


@app.post("/config-builder/build")
async def config_builder_build(request: Request):
    """Build the 168-byte ICD §4 envelope from the form and download it."""
    from fastapi import Response
    form = await request.form()
    try:
        env, fname = _cfg_from_form(form)
    except ValueError as e:
        return TEMPLATES.TemplateResponse(
            request, "config_builder.html",
            {"error": str(e), "form": dict(form), "stats": db.stats()},
            status_code=400)
    return Response(
        env,
        media_type="application/octet-stream",
        headers={"Content-Disposition": f'attachment; filename="{fname}"'},
    )


# ------------------------- analytics & export -------------------------

@app.get("/api/analytics/{device_id}")
async def api_analytics(device_id: str, days: int = Query(30, le=365)):
    """Summary stats + per-day aggregates for the analytics page. The key
    may be a [device_groups] label: rows from all members are merged, so a
    multi-identity tracker summarizes as one."""
    from .analytics import summarize
    ids, _, _ = _resolve_device_key(device_id)
    since = int(time.time()) - days * 86400
    rows = []
    for i in ids:
        rows.extend(db.readings_all(i, since=since))
    rows.sort(key=lambda r: r["seen_at"] or 0)
    return {
        "days": days,
        "summary": summarize(rows),
        "daily": db.daily_agg(ids, days),
    }


def _csv_response(rows: list, columns: list, filename: str) -> Response:
    import csv
    import io
    buf = io.StringIO()
    w = csv.writer(buf)
    w.writerow(columns)
    for r in rows:
        w.writerow([r.get(c) for c in columns])
    return Response(
        buf.getvalue(), media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


_READING_COLS = ["seen_at", "gps_time", "device_id", "source", "temp_c",
                 "hum_pct", "pressure_hpa", "bat_mv", "lux", "acc_x_mg",
                 "acc_y_mg", "acc_z_mg", "latitude", "longitude", "altitude_m",
                 "sats", "gps_valid", "awake_s", "sleep_min", "gpio",
                 "raw_hex"]


def _epoch(params, key: str):
    v = params.get(key)
    if not v:
        return None
    return int(v)  # 400s bubble up as HTTPException via handler? keep simple


@app.get("/api/export/{device_id}.csv")
async def export_readings_csv(device_id: str,
                              since: Optional[int] = Query(None),
                              until: Optional[int] = Query(None)):
    rows = _export_rows(device_id, since, until)
    return _csv_response(rows, _READING_COLS,
                         f"minilora-{device_id}-readings.csv")


@app.get("/api/export/{device_id}.json")
async def export_readings_json(device_id: str,
                               since: Optional[int] = Query(None),
                               until: Optional[int] = Query(None)):
    rows = _export_rows(device_id, since, until)
    return JSONResponse({"device_id": device_id, "count": len(rows),
                         "readings": rows})


def _export_rows(key: str, since: Optional[int],
                 until: Optional[int]) -> list:
    """Readings for a device_id or a [device_groups] label (all members
    merged, time-sorted). Each row keeps its TRUE device_id — a mixed
    export stays traceable to its transport."""
    ids, _, _ = _resolve_device_key(key)
    rows = []
    for i in ids:
        rows.extend(db.readings_all(i, since=since, until=until))
    rows.sort(key=lambda r: r["seen_at"] or 0)
    for r in rows:
        r.setdefault("device_id", key)
    return rows


@app.get("/api/export/{device_id}/diagnostics.csv")
async def export_diagnostics_csv(device_id: str):
    ids, _, _ = _resolve_device_key(device_id)
    rows = []
    for i in ids:
        rows.extend(db.diagnostics(i, limit=1_000_000))
    rows.sort(key=lambda d: d["seen_at"] or 0)
    for r in rows:
        r.setdefault("device_id", device_id)
    cols = ["seen_at", "fw_version", "boot_count", "unexpected_resets",
            "reset_reason", "gps_timeouts", "lora_join_failures",
            "lora_send_failures", "wifi_failures", "zigbee_failures",
            "sensor_read_failures", "gps_consec_fails", "power_flags",
            "flags2", "last_ttff", "link_margin", "gw_cnt", "ota_status",
            "config_flags", "raw_hex"]
    return _csv_response(rows, cols, f"minilora-{device_id}-diagnostics.csv")


@app.get("/api/export/db")
async def export_db_backup():
    """Consistent SQLite snapshot of the whole database."""
    import os
    import tempfile
    from fastapi.responses import FileResponse
    from starlette.background import BackgroundTask
    fd, path = tempfile.mkstemp(suffix=".sqlite3")
    os.close(fd)
    db.backup_to(path)
    return FileResponse(
        path, media_type="application/octet-stream",
        filename="minilora-server-db.sqlite3",
        background=BackgroundTask(os.unlink, path))


@app.get("/device/{device_id}/analytics", response_class=HTMLResponse)
async def ui_analytics(request: Request, device_id: str):
    return TEMPLATES.TemplateResponse(request, "analytics.html", {
        "device_id": device_id,
        "stats": db.stats(),
    })


# ------------------------- web UI -------------------------

def _h(ts) -> str:
    """Human-readable local time for a unix epoch ('' for absent/0)."""
    try:
        return datetime.fromtimestamp(int(ts)).strftime("%Y-%m-%d %H:%M:%S") \
            if ts else ""
    except (TypeError, ValueError, OSError, OverflowError):
        return ""


@app.get("/", response_class=HTMLResponse)
async def ui_dashboard(request: Request):
    devices = []
    for d in db.devices():
        d = dict(d)
        d["last_seen_h"] = _h(d["last_seen"])
        devices.append(d)
    # Physical-device grouping ([device_groups] in config.toml): a tracker
    # that reports under several identities is ONE row with transport chips
    # instead of two confusing lookalikes. Presentational only.
    by_id = {d["device_id"]: d for d in devices}
    entries: list = []
    claimed: set = set()
    for label, ids in cfg.device_groups.items():
        members = [by_id[i] for i in ids if i in by_id]
        if not members:
            continue
        last = max(m["last_seen"] for m in members)
        primary = max(members, key=lambda m: m["last_seen"])["device_id"]
        entries.append({"kind": "group", "label": label, "members": members,
                        "primary": primary, "last_seen_h": _h(last)})
        claimed.update(i for i in ids if i in by_id)
    for d in devices:
        if d["device_id"] not in claimed:
            entries.append({"kind": "device", "label": d["device_id"],
                            "members": [d], "primary": d["device_id"],
                            "last_seen_h": d["last_seen_h"]})
    return TEMPLATES.TemplateResponse(request, "dashboard.html", {
        "entries": entries,
        "stats": db.stats(),
    })


def _resolve_device_key(key: str) -> tuple:
    """A page/API key is either a device_id or a [device_groups] label.
    Returns (member_ids, display_name, is_group). An exact device_id wins
    over a group label sharing the same name — the per-identity chips on
    the dashboard must keep opening the identity, not the group."""
    if key not in cfg.device_groups:
        return [key], key, False
    known = {d["device_id"] for d in db.devices()}
    ids = [i for i in cfg.device_groups[key] if i in known]
    if not ids:  # configured but no member has reported yet
        ids = list(cfg.device_groups[key])
    return ids, key, True


def _ttn_target(ids: list) -> str:
    """Downlink target for a resolved key: the group member the TTN
    transport actually talks to (registry last_source), else the first id
    through _resolve_ttn_device's single-TTN-device fallback."""
    rows = {d["device_id"]: dict(d) for d in db.devices()}
    for i in ids:
        if (rows.get(i) or {}).get("last_source") == "ttn":
            return i
    return _resolve_ttn_device(ids[0])


def _device_ctx(key: str) -> dict:
    """Shared loading for every device sub-page (overview/charts/remote/
    data). The key is a device_id OR a [device_groups] label: group keys
    merge every member's rows into ONE time-sorted view, so a physical
    tracker reporting under several identities (WiFi push name + TTN id)
    reads as a single story instead of two half-truths."""
    ids, name, is_group = _resolve_device_key(key)
    rows = []
    for i in ids:
        rows.extend(db.readings(i, limit=1000))
    rows.sort(key=lambda r: r["seen_at"] or 0)
    rows = rows[-1000:]  # newest 1000 across members, oldest first
    diags = []
    for i in ids:
        diags.extend(db.diagnostics(i, limit=50))
    diags.sort(key=lambda d: d["seen_at"] or 0, reverse=True)
    diags = diags[:50]
    for r in rows:
        r["seen_h"] = _h(r["seen_at"])
        r["gps_h"] = _h(r["gps_time"])
    # Remote-config states (ICD 3.5): from the newest diag that carries the
    # 0x05 frame. on = None → unknown (no 0x05 diag yet).
    cf = next((d["config_flags"] for d in diags
               if d["config_flags"] is not None), None)
    masks = [("wifi", "WiFi", 0x01), ("zigbee", "Zigbee", 0x02),
             ("ble", "BLE beacon", 0x04), ("gnss", "GNSS", 0x08),
             ("bme280", "BME280", 0x10), ("opt3001", "OPT3001", 0x20),
             ("lis3dh", "LIS3DH", 0x40)]
    cfg_rows = [{"name": n, "label": label,
                 "on": None if cf is None else bool(cf & m)}
                for n, label, m in masks]
    dev_rows = {d["device_id"]: dict(d) for d in db.devices()}
    return {
        "device_id": name,
        "is_group": is_group,
        "group_members": [
            {"device_id": i,
             "last_source": (dev_rows.get(i) or {}).get("last_source")}
            for i in ids] if is_group else [],
        "readings": rows,
        "readings_json": _json_rows(rows),
        "diags": diags,
        "cfg_rows": cfg_rows,
        "cfg_known": cf is not None,
        "sleep_table": SLEEP_MINUTES,
        "latest": rows[-1] if rows else None,
    }


@app.get("/device/{device_id}", response_class=HTMLResponse)
async def ui_device(request: Request, device_id: str):
    """Overview: the latest-reading hero cards + the position map."""
    return TEMPLATES.TemplateResponse(request, "device.html", {
        "stats": db.stats(), **_device_ctx(device_id),
    })


@app.get("/device/{device_id}/charts", response_class=HTMLResponse)
async def ui_device_charts(request: Request, device_id: str):
    """The five telemetry timeseries."""
    return TEMPLATES.TemplateResponse(request, "charts.html", {
        "stats": db.stats(), **_device_ctx(device_id),
    })


@app.get("/device/{device_id}/remote", response_class=HTMLResponse)
async def ui_device_remote(request: Request, device_id: str):
    """Remote control: fPort-3 commands + remote config + sleep duration."""
    return TEMPLATES.TemplateResponse(request, "remote.html", {
        "stats": db.stats(), **_device_ctx(device_id),
    })


@app.get("/device/{device_id}/data", response_class=HTMLResponse)
async def ui_device_data(request: Request, device_id: str):
    """Maintenance (fetch/prune/delete) + the readings table."""
    return TEMPLATES.TemplateResponse(request, "data.html", {
        "stats": db.stats(), **_device_ctx(device_id),
    })


def _json_rows(rows: list) -> str:
    import json
    slim = [{k: r[k] for k in ("seen_at", "temp_c", "hum_pct", "pressure_hpa",
                               "bat_mv", "lux", "acc_x_mg", "acc_y_mg",
                               "acc_z_mg", "latitude", "longitude",
                               "sats", "gps_valid", "source")} for r in rows]
    return json.dumps(slim)


@app.get("/device/{device_id}/diagnostics", response_class=HTMLResponse)
async def ui_diag(request: Request, device_id: str):
    diags = []
    for d in db.diagnostics(device_id, limit=500):
        d = dict(d)
        d["seen_h"] = _h(d["seen_at"])
        diags.append(d)
    return TEMPLATES.TemplateResponse(request, "diagnostics.html", {
        "stats": db.stats(),
        "device_id": device_id,
        "diags": diags,
    })


# ------------------------- data maintenance -------------------------

# Allowed prune windows for /api/device/{id}/prune?older_than=…
PRUNE_CHOICES = {"1h": 3600, "1d": 86400, "1w": 7 * 86400, "30d": 30 * 86400}


@app.post("/api/device/{device_id}/prune")
async def api_prune(device_id: str, older_than: str = Query("1d")):
    """Delete readings/diagnostics older than the chosen window
    (1h|1d|1w|30d), or keep only the newest reading+diagnostic with
    older_than=all_but_last. The key may be a [device_groups] label — every
    member is pruned. The device registry rows are kept; the devices
    re-populate on their next uplink. Destructive and unauthenticated
    (LAN tool) — like all routes."""
    ids, _, is_group = _resolve_device_key(device_id)
    if older_than == "all_but_last":
        deleted = sum(db.prune_all_but_last(i) for i in ids)
        log.info("prune %s all_but_last: %d rows deleted",
                 device_id, deleted)
        return {"device_id": device_id, "older_than": older_than,
                "deleted": deleted}
    seconds = PRUNE_CHOICES.get(older_than)
    if seconds is None:
        raise HTTPException(
            400, f"older_than must be one of "
                 f"{sorted(PRUNE_CHOICES) + ['all_but_last']}")
    deleted = sum(db.prune_before(i, seconds) for i in ids)
    log.info("prune %s older_than=%s: %d rows deleted",
             device_id, older_than, deleted)
    out = {"device_id": device_id, "older_than": older_than,
           "deleted": deleted}
    if is_group:
        out["members"] = ids
    return out


@app.delete("/api/device/{device_id}")
async def api_delete_device(device_id: str):
    """Remove the device and ALL its data (readings, diagnostics, registry).
    No undo — the UI asks for confirmation. If the device reports again it
    simply appears as a fresh entry."""
    deleted = db.delete_device(device_id)
    log.info("delete device %s: %d data rows removed", device_id, deleted)
    return {"device_id": device_id, "deleted_rows": deleted}


@app.post("/device/{device_id}/delete")
async def ui_delete_device(device_id: str):
    """HTML-form variant of the delete (dashboard button) — redirects home."""
    db.delete_device(device_id)
    log.info("delete device %s (ui)", device_id)
    return RedirectResponse("/", status_code=303)



# ------------------------- OTA (device-pull) -------------------------

FW_DIR = Path(cfg.server.db_path).parent / "firmware"
FW_DIR.mkdir(parents=True, exist_ok=True)


def _fw_path(fw_id: int) -> Path:
    return FW_DIR / f"{fw_id}.bin"


@app.get("/ota/check")
async def ota_check(device: str = Query(...), current: str = Query("")):
    """Device-pull check: newest active image for this device (or ANY),
    version != current. 404 when unknown device / nothing to do."""
    fw = db.find_update(device, current)
    if not fw:
        return {"update": False}
    return {
        "update": True,
        "version": fw["version"],
        "size": fw["size"],
        "sha256": fw["sha256"],
        "path": f"/ota/download/{fw['id']}",
    }


@app.get("/ota/download/{fw_id}")
async def ota_download(fw_id: int):
    from fastapi.responses import FileResponse
    row = next((f for f in db.list_firmware() if f["id"] == fw_id), None)
    if not row:
        raise HTTPException(404, "unknown firmware")
    path = _fw_path(fw_id)
    if not path.exists():
        raise HTTPException(404, "firmware file missing")
    return FileResponse(path, media_type="application/octet-stream",
                        filename=f"minilora-{row['version']}.bin")


def _sanitize_version(v: str) -> str:
    v = (v or "").strip()
    return v if re.fullmatch(r"[0-9A-Za-z._-]{1,16}", v) else "0.0.0"


@app.post("/api/ota/upload")
async def ota_upload(request: Request,
                     file: UploadFile = File(...),
                     version: str = Form(...),
                     device_id: str = Form(None),
                     token: str = Query(None)):
    """Upload a .bin (idf.py build output). Activate it separately."""
    check_push_token(request, token)
    data = await file.read()
    if len(data) < 100_000 or len(data) > 0x1D0000:
        raise HTTPException(400, f"implausible size {len(data)} (slot is 0x1D0000)")
    sha = hashlib.sha256(data).hexdigest()
    # empty/omitted device_id = NULL = any device may pull this image
    # (the device checks /ota/check as its EUI — see find_update)
    fw_id = db.add_firmware(_sanitize_version(version),
                            (device_id or "").strip() or None,
                            file.filename or "image.bin", len(data), sha)
    _fw_path(fw_id).write_bytes(data)
    log.info("OTA upload: fw %s for %s (%d bytes, %s)", version,
             device_id or "ANY", len(data), sha[:12])
    return {"ok": True, "id": fw_id, "sha256": sha, "size": len(data)}


@app.post("/api/ota/activate/{fw_id}")
async def ota_activate(fw_id: int):
    if not db.activate_firmware(fw_id):
        raise HTTPException(404, "unknown firmware")
    return {"ok": True}


@app.delete("/api/ota/{fw_id}")
async def ota_delete(fw_id: int):
    name = db.delete_firmware(fw_id)
    if not name:
        raise HTTPException(404, "unknown firmware")
    _fw_path(fw_id).unlink(missing_ok=True)
    return {"ok": True}


@app.post("/api/ota/notify/{device_id}")
async def ota_notify(device_id: str):
    """Push the OTA_UPDATE command (0x05, fPort 3) via TTN so the device
    pulls on its next wake. Requires [ttn] api_key/app_id in config."""
    return await _send_downlink(device_id, bytes([0x4D, 0x05, 0x00]))


# ------------------------- remote commands (ICD §3, fPort 3 via TTN) -------------------------

# Named fPort-3 opcodes (ICD §3).
_DL_OPCODES = {"locate_now": 0x01, "send_diag": 0x02, "reset_diag": 0x03,
               "ble_window": 0x04, "ota_update": 0x05}
# ICD 3.5 §3 CFG_SET flag ids (arg = (value << 7) | id).
_CFG_FLAG_IDS = {"wifi": 1, "zigbee": 2, "ble": 3, "gnss": 4,
                 "bme280": 5, "opt3001": 6, "lis3dh": 7}


def _resolve_ttn_device(device_id: str) -> str:
    """Map a web device id to the TTN end-device id. TTN-path devices already
    carry the TTN id; WiFi-push devices (keyed by ?device=) don't — for those,
    fall back to the single configured TTN device."""
    if device_id in cfg.ttn.device_ids:
        return device_id
    if len(cfg.ttn.device_ids) == 1:
        return cfg.ttn.device_ids[0]
    raise HTTPException(400, f"cannot map device '{device_id}' to a TTN "
                             f"end-device (configured: {cfg.ttn.device_ids})")


async def _send_downlink(device_id: str, payload: bytes) -> dict:
    """Queue a raw downlink via TTN downlink/push (class A: it rides the RX
    window of the device's next uplink)."""
    if not (cfg.ttn.enabled and cfg.ttn.api_key and cfg.ttn.app_id):
        raise HTTPException(400, "TTN not configured in config.toml")
    import base64
    import httpx
    ttn_device = _resolve_ttn_device(device_id)
    region = cfg.ttn.region or "eu1"
    url = (f"https://{region}.cloud.thethings.network/api/v3/as/"
           f"applications/{cfg.ttn.app_id}/devices/{ttn_device}/down/push")
    body = {"downlinks": [{"f_port": 3, "priority": "NORMAL",
                           "frm_payload": base64.b64encode(payload).decode()}]}
    async with httpx.AsyncClient(timeout=15.0) as client:
        r = await client.post(url, json=body,
                              headers={"Authorization": f"Bearer {cfg.ttn.api_key}"})
    if r.status_code >= 300:
        raise HTTPException(502, f"TTN error {r.status_code}: {r.text[:200]}")
    return {"ok": True, "device": ttn_device,
            "hex": payload.hex(" ").upper()}


@app.post("/api/device/{device_id}/command")
async def api_send_command(device_id: str, cmd: str = Query(...),
                           flag: str = Query(None), on: bool = Query(True),
                           index: int = Query(None)):
    """Queue a remote command (fPort 3, ICD §3) for the device via TTN.

    cmd = locate_now | send_diag | reset_diag | ble_window | ota_update |
          config_set (with flag=<name> and on=<bool>, ICD 3.5 §3) |
          set_range (with index=<0..8>, ICD 3.6 §3 — the sleep-table index,
          same scale as the config envelope / the app's config tab).
    """
    if cmd in _DL_OPCODES:
        payload = bytes([0x4D, _DL_OPCODES[cmd], 0x00])
        note = cmd
    elif cmd == "config_set":
        flag_id = _CFG_FLAG_IDS.get((flag or "").lower())
        if flag_id is None:
            raise HTTPException(400, f"unknown flag '{flag}' (expected one "
                                     f"of {sorted(_CFG_FLAG_IDS)})")
        arg = ((1 if on else 0) << 7) | flag_id
        payload = bytes([0x4D, 0x06, arg])
        note = f"{flag} -> {'on' if on else 'off'}"
    elif cmd == "set_range":
        if index is None or not 0 <= index < len(SLEEP_MINUTES):
            raise HTTPException(400, f"set_range index must be 0.."
                                     f"{len(SLEEP_MINUTES) - 1}")
        payload = bytes([0x4D, 0x07, index])
        note = f"sleep {SLEEP_MINUTES[index]} min"
    else:
        raise HTTPException(400, f"unknown cmd '{cmd}'")
    # The key may be a [device_groups] label — resolve it to the member the
    # TTN transport actually talks to before queueing.
    target = _ttn_target(_resolve_device_key(device_id)[0])
    out = await _send_downlink(target, payload)
    out["note"] = note
    return out


@app.post("/api/device/{device_id}/downlink/clear")
async def api_clear_downlink(device_id: str):
    """Clear the device's TTN downlink queue (downlink/replace with an
    empty queue) — cancels pending commands from any surface. The key may
    be a [device_groups] label; it resolves to the TTN member."""
    if not (cfg.ttn.enabled and cfg.ttn.api_key and cfg.ttn.app_id):
        raise HTTPException(400, "TTN not configured in config.toml")
    import base64
    import httpx
    ttn_device = _ttn_target(_resolve_device_key(device_id)[0])
    region = cfg.ttn.region or "eu1"
    url = (f"https://{region}.cloud.thethings.network/api/v3/as/"
           f"applications/{cfg.ttn.app_id}/devices/{ttn_device}/down/replace")
    async with httpx.AsyncClient(timeout=15.0) as client:
        r = await client.post(url, json={"downlinks": []},
                              headers={"Authorization": f"Bearer {cfg.ttn.api_key}"})
    if r.status_code >= 300:
        raise HTTPException(502, f"TTN error {r.status_code}: {r.text[:200]}")
    return {"ok": True, "device": ttn_device, "cleared": True}


@app.get("/ota", response_class=HTMLResponse)
async def ui_ota(request: Request):
    firmware = []
    for f in db.list_firmware():
        f = dict(f)
        f["uploaded_h"] = _h(f.get("uploaded_at"))
        firmware.append(f)
    return TEMPLATES.TemplateResponse(request, "ota.html", {
        "stats": db.stats(),
        "firmware": firmware,
        "devices": [dict(r) for r in db.devices()],
        "ttn_ready": bool(cfg.ttn.enabled and cfg.ttn.api_key
                          and cfg.ttn.app_id),
    })


# ------------------------- entry -------------------------

_retention_task = None  # strong ref — asyncio keeps only weak ones


def _setup_logging() -> None:
    """Console always; plus a size-capped rotating file when
    [server].log_file is set (env MINILORA_LOG_FILE). The on-disk log is
    forever bounded at log_max_mb × (1 + log_backups)."""
    import logging.handlers
    handlers = [logging.StreamHandler()]
    if cfg.server.log_file:
        handlers.append(logging.handlers.RotatingFileHandler(
            cfg.server.log_file, maxBytes=cfg.server.log_max_mb * 1_000_000,
            backupCount=cfg.server.log_backups, encoding="utf-8"))
    fmt = logging.Formatter("%(asctime)s %(name)s %(levelname)s %(message)s")
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    for h in handlers:
        h.setFormatter(fmt)
        root.addHandler(h)


def enforce_retention() -> dict:
    """One retention pass: per-device newest-N caps, then the global DB
    ceiling (oldest-first, fair across devices). Readings cost ~200 B/row
    on disk (measured, indexes + diag share included); the 150 B/row
    estimate here deletes slightly early — the next hourly pass converges.
    Returns what it did (logged, and used by the tests)."""
    if not cfg.retention.enabled:
        return {}
    out: dict = {}
    for d in db.devices():
        dev = d["device_id"]
        cut_r = db.prune_keep_newest(
            "readings", dev, cfg.retention.readings_per_device)
        cut_d = db.prune_keep_newest(
            "diagnostics", dev, cfg.retention.diagnostics_per_device)
        if cut_r or cut_d:
            out[dev] = {"readings": cut_r, "diagnostics": cut_d}
    cap = cfg.retention.db_max_mb * 1_000_000
    size = db.size_bytes()
    if size > cap:
        n = int((size - cap) / 150) + 1
        cut_r = db.prune_oldest_global("readings", n)
        cut_d = db.prune_oldest_global("diagnostics", n // 16 + 1)
        out["global"] = {"readings": cut_r, "diagnostics": cut_d}
    deleted = sum(v["readings"] + v["diagnostics"] for v in out.values()
                  if isinstance(v, dict))
    if deleted > 20_000:
        db.vacuum()  # hand freed pages back: ~0.1 s per 100 MB, rare
        out["vacuumed"] = True
    if out:
        log.info("retention pass: %s", out)
    return out


async def _retention_loop() -> None:
    while True:
        try:
            await asyncio.to_thread(enforce_retention)
        except Exception:
            log.exception("retention pass failed")
        await asyncio.sleep(cfg.retention.check_interval_s)


def _ensure_log_file_handler() -> None:
    """Startup backstop for `uvicorn minilora_server.main:app` runs —
    main() already installed the handlers for the documented
    `python -m minilora_server.main` path (the Docker CMD)."""
    if not cfg.server.log_file:
        return
    import logging.handlers
    root = logging.getLogger()
    if any(isinstance(h, logging.handlers.RotatingFileHandler)
           for h in root.handlers):
        return
    h = logging.handlers.RotatingFileHandler(
        cfg.server.log_file, maxBytes=cfg.server.log_max_mb * 1_000_000,
        backupCount=cfg.server.log_backups, encoding="utf-8")
    h.setFormatter(logging.Formatter(
        "%(asctime)s %(name)s %(levelname)s %(message)s"))
    root.addHandler(h)
    acc = logging.getLogger("uvicorn.access")
    if not acc.propagate:       # uvicorn's default config swallows it
        acc.addHandler(h)


_poller_task = None  # strong ref — asyncio keeps only weak ones
_poller = None       # the TTNPoller instance (for /api/ttn/fetch-now)
_fetch_lock = None   # created in startup: manual fetches don't overlap


@app.on_event("startup")
async def startup():
    global _poller_task, _poller, _fetch_lock, _retention_task
    _poller = None
    _fetch_lock = asyncio.Lock()

    # ---- retention (keep the DB bounded for set-and-forget LAN use) ----
    _ensure_log_file_handler()
    if cfg.retention.enabled:
        _retention_task = asyncio.create_task(_retention_loop())
        log.info("retention: %d readings/device, DB cap %d MB, every %d s",
                 cfg.retention.readings_per_device, cfg.retention.db_max_mb,
                 cfg.retention.check_interval_s)

    # ---- Home Assistant bridge (MQTT discovery) ----
    if cfg.ha.enabled and cfg.ha.broker:
        try:
            from minilora_server.ha import HAPublisher
            ha_publisher = HAPublisher(cfg.ha)
            db.on_reading = _ha_on_reading
            db.on_diagnostic = _ha_on_diagnostic
            ha_publisher._on_connected = _ha_backfill
            ha_publisher.start()
            log.info("HA bridge started (%s:%d)", cfg.ha.broker, cfg.ha.port)
        except Exception:
            log.exception("HA bridge failed to start")
            ha_publisher = None
    else:
        log.info("HA bridge disabled ([ha] enabled=false or no broker)")

    # ---- TTN storage-API poller ----
    if not (cfg.ttn.enabled and cfg.ttn.api_key and cfg.ttn.device_ids):
        log.info("TTN poller disabled ([ttn] enabled=false or incomplete)")
        return
    poller = TTNPoller(
        cfg.ttn.region, cfg.ttn.app_id, cfg.ttn.api_key,
        cfg.ttn.device_ids, cfg.ttn.poll_interval_s,
        cfg.ttn.poll_limit, cfg.ttn.poll_window_h,
        on_frame=_store_polled,
    )
    _poller = poller
    # Keeping the reference matters: an unreferenced task can be
    # garbage-collected before it ever runs (this silently killed the
    # poller once — no log line, no polls, nothing).
    _poller_task = asyncio.create_task(poller.run_forever())


@app.post("/api/ttn/fetch-now")
async def api_fetch_now():
    """Manual poller run — the app's 'fetch now' twin: pull the last
    poll_window_h from TTN right now instead of waiting for the next
    interval tick. Exact dedup makes this idempotent; returns the number
    of NEW frames stored."""
    if not _poller:
        raise HTTPException(
            503, "TTN poller not running ([ttn] disabled or incomplete)")
    async with _fetch_lock:   # don't overlap with the background loop
        stored = await _poller.poll_once()
    log.info("manual TTN fetch: %d new frame(s) stored", stored)
    return {"stored": stored}


# ------------------------- Home Assistant bridge -------------------------

ha_publisher = None  # created at startup when [ha] enabled


def _ha_on_reading(device_id: str, source: str, decoded: dict,
                   seen_at: int) -> None:
    """db.on_reading hook — runs on the ingest thread; paho handles the IO."""
    if ha_publisher:
        ha_publisher.publish_state(device_id, decoded, source, seen_at)


def _ha_on_diagnostic(device_id: str, decoded: dict, seen_at: int) -> None:
    if ha_publisher:
        ha_publisher.publish_diagnostic(device_id, decoded)


def _ha_backfill() -> None:
    """Push discovery + the latest state for every known device."""
    for d in db.devices():
        dev = d["device_id"]
        ha_publisher.discover_device(dev)
        latest = db.readings(dev, limit=1)
        if latest:
            r = latest[0]
            ha_publisher.publish_state(dev, r, r["source"], r["seen_at"])


def _history_as_reading(entry: dict) -> dict:
    """Adapt a history entry to the insert_reading field set."""
    return {
        "temp_c": 0.0, "hum_pct": 0, "pressure_hpa": 0, "bat_mv": 0,
        "lux": 0, "acc_mg": [0, 0, 0],
        "gps_valid": True,
        "latitude": entry["latitude"], "longitude": entry["longitude"],
        "altitude_m": 0, "satellites": entry["satellites"],
        "gps_time": entry["gps_time"], "awake_s": 0, "sleep_min": 0,
        "gpio_led_green": False, "gpio_button2": False,
    }


def _store_polled(device_id: str, f_port: int, data: bytes,
                  frame_ts: int = None) -> int:
    """Store a polled TTN frame. Dedup keys:
    - telemetry/diag: the uplink's own received_at (frame_ts) + raw_hex —
      exact, so re-polling a 12 h window never duplicates;
    - history: the payload's own fix epoch per entry (gps_time), falling
      back to frame_ts + entry index when the fix time is unknown.
    Returns the number of NEW rows stored (for the fetch-now feedback)."""
    # Respect the prune watermark: frames the user explicitly deleted
    # must not trickle back from TTN's 12 h storage window.
    wm = db.frames_watermark(device_id)
    if frame_ts is not None and wm and frame_ts < wm:
        return 0
    kind, decoded = decode_frame(f_port, data)
    raw_hex = data.hex()
    if kind == "telemetry":
        if frame_ts is not None:
            if db.has_frame(device_id, "ttn", frame_ts, raw_hex):
                return 0
        elif db.has_duplicate(device_id, "ttn", raw_hex):
            return 0  # no received_at: legacy 60-s heuristic
        db.insert_reading(device_id, "ttn", decoded, raw_hex,
                          seen_at=frame_ts, frame_ts=frame_ts)
        log.info("ttn poll: %s telemetry stored", device_id)
        return 1
    elif kind == "history":
        stored = 0
        for i, entry in enumerate(decoded["entries"]):
            if entry["gps_time"]:
                if db.has_history_fix(device_id, entry["gps_time"]):
                    continue
                db.insert_reading(device_id, "ttn-hist",
                                  _history_as_reading(entry), raw_hex,
                                  seen_at=entry["gps_time"])
                stored += 1
            else:
                entry_hex = f"{raw_hex}#{i}"  # index-tagged: dedupable
                if frame_ts is not None and db.has_frame(
                        device_id, "ttn-hist", frame_ts, entry_hex):
                    continue
                db.insert_reading(device_id, "ttn-hist",
                                  _history_as_reading(entry), entry_hex,
                                  seen_at=frame_ts, frame_ts=frame_ts)
                stored += 1
        if stored:
            log.info("ttn poll: %s history (%d new entries) stored",
                     device_id, stored)
        return stored
    else:
        if frame_ts is not None:
            if db.has_diagnostic(device_id, frame_ts, raw_hex):
                return 0
        elif db.has_duplicate(device_id, "ttn", raw_hex):
            return 0  # no received_at: legacy 60-s heuristic
        db.insert_diagnostic(device_id, decoded, raw_hex, seen_at=frame_ts)
        log.info("ttn poll: %s diagnostics stored", device_id)
        return 1


def main():
    _setup_logging()
    # log_config=None: keep OUR handlers — uvicorn's default dictConfig
    # would replace them. Its loggers then propagate to root, so the
    # rotating file catches access logs too.
    uvicorn.run(app, host=cfg.server.host, port=cfg.server.port,
                log_level="info", log_config=None)


if __name__ == "__main__":
    main()
