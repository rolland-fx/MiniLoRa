"""SQLite storage (WAL) — readings, diagnostics, devices."""

import logging
import sqlite3
import threading
import time
from pathlib import Path

log = logging.getLogger("minilora.db")

_SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    source TEXT NOT NULL,
    seen_at INTEGER NOT NULL,
    gps_time INTEGER,
    temp_c REAL, hum_pct INTEGER, pressure_hpa REAL,
    bat_mv INTEGER, lux INTEGER,
    acc_x_mg REAL, acc_y_mg REAL, acc_z_mg REAL,
    latitude REAL, longitude REAL, altitude_m REAL,
    sats INTEGER, gps_valid INTEGER,
    awake_s INTEGER, sleep_min INTEGER, gpio INTEGER,
    raw_hex TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_readings_dev_seen ON readings(device_id, seen_at);
CREATE TABLE IF NOT EXISTS prune_watermarks (
    device_id TEXT PRIMARY KEY,
    before_ts INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS diagnostics (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    seen_at INTEGER NOT NULL,
    fw_version TEXT,
    boot_count INTEGER, unexpected_resets INTEGER, reset_reason INTEGER,
    gps_timeouts INTEGER, lora_join_failures INTEGER, lora_send_failures INTEGER,
    wifi_failures INTEGER, zigbee_failures INTEGER, sensor_read_failures INTEGER,
    gps_consec_fails INTEGER, power_flags INTEGER, flags2 INTEGER,
    last_ttff INTEGER, link_margin INTEGER, gw_cnt INTEGER, ota_status INTEGER,
    config_flags INTEGER,
    raw_hex TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_diag_dev_seen ON diagnostics(device_id, seen_at);

CREATE TABLE IF NOT EXISTS devices (
    device_id TEXT PRIMARY KEY,
    first_seen INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    last_source TEXT
);

CREATE TABLE IF NOT EXISTS firmware (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    version TEXT NOT NULL,
    device_id TEXT,                -- NULL = any device
    filename TEXT NOT NULL,
    size INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    active INTEGER NOT NULL DEFAULT 0,
    uploaded_at INTEGER NOT NULL
);
"""


class Database:
    def __init__(self, path: str):
        self._path = path
        self._lock = threading.Lock()
        # Optional post-insert hooks (set by main.py to the HA bridge).
        # Signature: on_reading(device_id, source, decoded, seen_at),
        #            on_diagnostic(device_id, decoded, seen_at)
        self.on_reading = None
        self.on_diagnostic = None
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self._conn = sqlite3.connect(path, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.executescript(_SCHEMA)
        # migrations for DBs created before diag v4
        for col in ("last_ttff INTEGER", "link_margin INTEGER",
                    "gw_cnt INTEGER", "ota_status INTEGER",
                    "config_flags INTEGER"):  # diag v5 (ICD 3.5)
            try:
                self._conn.execute(f"ALTER TABLE diagnostics ADD COLUMN {col}")
            except sqlite3.OperationalError:
                pass  # column exists
        # TTN storage-API re-polls need the uplink's own timestamp to dedup
        # exactly (raw_hex alone is ambiguous; the 60-s window is too short
        # for a 12 h catch-up window).
        try:
            self._conn.execute("ALTER TABLE readings ADD COLUMN frame_ts INTEGER")
        except sqlite3.OperationalError:
            pass  # column exists
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.commit()

    # ---- writes ----

    def insert_reading(self, device_id: str, source: str, decoded: dict,
                       raw_hex: str, seen_at: int = None,
                       frame_ts: int = None) -> None:
        seen = seen_at or int(time.time())
        # Defense in depth against corrupted NMEA dates (2026-11-03 06:05:
        # one frame shipped a 2068 epoch): a GPS timestamp from the future
        # is garbage — stored as NULL, the receive time stays authoritative.
        gt = decoded.get("gps_time")
        if gt and gt > seen + 600:
            log.warning("gps_time %d is > 10 min after seen_at %d - dropped",
                        gt, seen)
            decoded = {**decoded, "gps_time": None}
        with self._lock, self._conn:
            self._conn.execute(
                """INSERT INTO readings
                   (device_id, source, seen_at, frame_ts, gps_time, temp_c,
                    hum_pct, pressure_hpa, bat_mv, lux, acc_x_mg, acc_y_mg,
                    acc_z_mg, latitude, longitude, altitude_m, sats,
                    gps_valid, awake_s, sleep_min, gpio, raw_hex)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (device_id, source, seen, frame_ts, decoded.get("gps_time"),
                 decoded["temp_c"], decoded["hum_pct"], decoded["pressure_hpa"],
                 decoded["bat_mv"], decoded["lux"],
                 *decoded["acc_mg"], decoded["latitude"], decoded["longitude"],
                 decoded["altitude_m"], decoded["satellites"],
                 int(decoded["gps_valid"]), decoded["awake_s"],
                 decoded["sleep_min"],
                 (0x01 if decoded["gpio_led_green"] else 0) |
                 (0x02 if decoded["gpio_button2"] else 0),
                 raw_hex))
            self._touch_device(device_id, source, seen)
        hook = self.on_reading
        if hook:
            try:
                hook(device_id, source, decoded, seen)
            except Exception:
                log.exception("on_reading hook failed")

    def insert_diagnostic(self, device_id: str, decoded: dict, raw_hex: str,
                          seen_at: int = None,
                          source: str = "ttn-diag") -> None:
        """source labels the registry touch (devices.last_source): the WiFi
        push endpoint passes "wifi-diag", TTN arrivals keep "ttn-diag"."""
        seen = seen_at or int(time.time())
        with self._lock, self._conn:
            self._conn.execute(
                """INSERT INTO diagnostics
                   (device_id, seen_at, fw_version, boot_count, unexpected_resets,
                    reset_reason, gps_timeouts, lora_join_failures,
                    lora_send_failures, wifi_failures, zigbee_failures,
                    sensor_read_failures, gps_consec_fails, power_flags,
                    flags2, last_ttff, link_margin, gw_cnt, ota_status,
                    config_flags, raw_hex)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (device_id, seen, decoded["fw_version"], decoded["boot_count"],
                 decoded["unexpected_resets"], decoded["reset_reason"],
                 decoded["gps_timeouts"], decoded["lora_join_failures"],
                 decoded["lora_send_failures"], decoded["wifi_failures"],
                 decoded["zigbee_failures"], decoded["sensor_read_failures"],
                 decoded["gps_consec_fails"], decoded["power_flags"],
                 decoded["flags2"], decoded.get("last_ttff"),
                 decoded.get("link_margin"), decoded.get("gw_cnt"),
                 decoded.get("ota_status"), decoded.get("config_flags"),
                 raw_hex))
            self._touch_device(device_id, source, seen)
        hook = self.on_diagnostic
        if hook:
            try:
                hook(device_id, decoded, seen)
            except Exception:
                log.exception("on_diagnostic hook failed")

    def _touch_device(self, device_id: str, source: str, seen: int) -> None:
        self._conn.execute(
            """INSERT INTO devices (device_id, first_seen, last_seen, last_source)
               VALUES (?,?,?,?)
               ON CONFLICT(device_id) DO UPDATE SET
                 last_seen=excluded.last_seen, last_source=excluded.last_source""",
            (device_id, seen, seen, source))

    def has_duplicate(self, device_id: str, source: str, raw_hex: str,
                      window_s: int = 60) -> bool:
        """True if the same frame was already stored recently (poller dedup)."""
        row = self._conn.execute(
            """SELECT 1 FROM readings WHERE device_id=? AND source=? AND raw_hex=?
               AND seen_at > ? LIMIT 1""",
            (device_id, source, raw_hex, int(time.time()) - window_s)).fetchone()
        return row is not None

    def has_diag_duplicate(self, device_id: str, raw_hex: str,
                           window_s: int = 60) -> bool:
        """Diag-frame retry dedup — has_duplicate's twin for the diagnostics
        table (the WiFi /diag endpoint; device retries must not double-store)."""
        row = self._conn.execute(
            """SELECT 1 FROM diagnostics WHERE device_id=? AND raw_hex=?
               AND seen_at > ? LIMIT 1""",
            (device_id, raw_hex, int(time.time()) - window_s)).fetchone()
        return row is not None

    def has_frame(self, device_id: str, source: str, frame_ts: int,
                  raw_hex: str) -> bool:
        """Exact-uplink dedup: TTN's received_at is unique per uplink, so the
        same frame reappearing in a re-polled window is recognized even days
        later — unlike the 60-s has_duplicate heuristic."""
        row = self._conn.execute(
            """SELECT 1 FROM readings WHERE device_id=? AND source=?
               AND frame_ts=? AND raw_hex=? LIMIT 1""",
            (device_id, source, frame_ts, raw_hex)).fetchone()
        return row is not None

    def has_history_fix(self, device_id: str, gps_time: int) -> bool:
        """True if an fPort-4 history entry with this fix time is stored.
        The payload's own fix epoch is the natural per-entry key — the same
        fix re-sent in overlapping history frames must not duplicate."""
        row = self._conn.execute(
            """SELECT 1 FROM readings WHERE device_id=? AND source='ttn-hist'
               AND gps_time=? LIMIT 1""",
            (device_id, gps_time)).fetchone()
        return row is not None

    def has_diagnostic(self, device_id: str, seen_at: int,
                       raw_hex: str) -> bool:
        """Exact diag-frame dedup: has_frame() checks the readings table,
        but diag frames live in diagnostics — checking the wrong table made
        every poll re-insert the same frames (duplicate storm 2026-11-02)."""
        row = self._conn.execute(
            """SELECT 1 FROM diagnostics WHERE device_id=? AND seen_at=?
               AND raw_hex=? LIMIT 1""",
            (device_id, seen_at, raw_hex)).fetchone()
        return row is not None

    # ---- maintenance ----

    def delete_device(self, device_id: str) -> int:
        """Remove a device and ALL its data: readings, diagnostics and the
        registry row. Returns the number of data rows deleted. There is no
        undo — the dashboard asks for confirmation before calling this."""
        with self._lock, self._conn:
            n = self._conn.execute(
                "DELETE FROM readings WHERE device_id=?",
                (device_id,)).rowcount
            n += self._conn.execute(
                "DELETE FROM diagnostics WHERE device_id=?",
                (device_id,)).rowcount
            self._conn.execute(
                "DELETE FROM devices WHERE device_id=?", (device_id,))
            return n

    def frames_watermark(self, device_id: str) -> int:
        """Frames older than this epoch were explicitly pruned — the poller
        must not re-import them from TTN's 12 h storage window (otherwise
        every all_but_last prune is undone within one poll tick)."""
        row = self._conn.execute(
            "SELECT before_ts FROM prune_watermarks WHERE device_id=?",
            (device_id,)).fetchone()
        return row[0] if row else 0

    def _advance_watermark(self, device_id: str, cutoff: int) -> None:
        with self._lock, self._conn:
            self._conn.execute(
                """INSERT INTO prune_watermarks (device_id, before_ts)
                   VALUES (?, ?)
                   ON CONFLICT(device_id) DO UPDATE
                   SET before_ts = MAX(before_ts, excluded.before_ts)""",
                (device_id, int(cutoff)))

    def prune_before(self, device_id: str, age_s: int) -> int:
        """Delete this device's readings and diagnostics older than age_s
        seconds. The registry row is kept (the device still exists and will
        re-populate the list on its next uplink). Returns rows deleted."""
        cutoff = int(time.time()) - age_s
        with self._lock, self._conn:
            n = self._conn.execute(
                "DELETE FROM readings WHERE device_id=? AND seen_at < ?",
                (device_id, cutoff)).rowcount
            n += self._conn.execute(
                "DELETE FROM diagnostics WHERE device_id=? AND seen_at < ?",
                (device_id, cutoff)).rowcount
        # Frames older than the cutoff are gone on purpose — the poller's
        # 12 h re-fetch window must not resurrect them.
        self._advance_watermark(device_id, cutoff)
        return n

    def prune_all_but_last(self, device_id: str) -> int:
        """Delete all of the device's data except the single most recent
        reading and the single most recent diagnostic (each table keeps
        its own newest row). The registry row is kept. Returns the number
        of data rows deleted."""
        with self._lock, self._conn:
            n = self._conn.execute(
                """DELETE FROM readings WHERE device_id=? AND id NOT IN (
                       SELECT id FROM readings WHERE device_id=?
                       ORDER BY seen_at DESC, id DESC LIMIT 1)""",
                (device_id, device_id)).rowcount
            n += self._conn.execute(
                """DELETE FROM diagnostics WHERE device_id=? AND id NOT IN (
                       SELECT id FROM diagnostics WHERE device_id=?
                       ORDER BY seen_at DESC, id DESC LIMIT 1)""",
                (device_id, device_id)).rowcount
            # Watermark at the kept reading's frame_ts (fall back to
            # seen_at): TTN frames older than that were just deleted on
            # purpose and must not be re-imported by the poller.
            kept = self._conn.execute(
                """SELECT COALESCE(frame_ts, seen_at) FROM readings
                   WHERE device_id=? ORDER BY seen_at DESC, id DESC LIMIT 1""",
                (device_id,)).fetchone()
        if kept and kept[0]:
            self._advance_watermark(device_id, kept[0])
        return n

    # ---- reads ----

    def devices(self) -> list:
        return self._conn.execute(
            "SELECT * FROM devices ORDER BY last_seen DESC").fetchall()

    def readings(self, device_id: str, limit: int = 500,
                 since: int = None) -> list:
        q = "SELECT * FROM readings WHERE device_id=?"
        args: list = [device_id]
        if since:
            q += " AND seen_at > ?"
            args.append(since)
        q += " ORDER BY seen_at DESC LIMIT ?"
        args.append(limit)
        return [dict(r) for r in self._conn.execute(q, args).fetchall()]

    def diagnostics(self, device_id: str, limit: int = 100) -> list:
        return [dict(r) for r in self._conn.execute(
            """SELECT * FROM diagnostics WHERE device_id=?
               ORDER BY seen_at DESC LIMIT ?""",
            (device_id, limit)).fetchall()]

    # ---- analytics / export ----

    def readings_all(self, device_id: str, since: int = None,
                     until: int = None) -> list:
        """Full range, ASC — export & analytics (no LIMIT)."""
        q = "SELECT * FROM readings WHERE device_id=?"
        args: list = [device_id]
        if since:
            q += " AND seen_at >= ?"
            args.append(since)
        if until:
            q += " AND seen_at <= ?"
            args.append(until)
        q += " ORDER BY seen_at ASC"
        return [dict(r) for r in self._conn.execute(q, args).fetchall()]

    def daily_agg(self, device_id, days: int = 30) -> list:
        """Per-UTC-day aggregates for the analytics charts. device_id is one
        id or a list — a [device_groups] page aggregates the union so a
        multi-identity tracker charts as one series."""
        ids = [device_id] if isinstance(device_id, str) else list(device_id)
        since = int(time.time()) - days * 86400
        q = """SELECT date(seen_at, 'unixepoch') AS day,
                      COUNT(*) AS n,
                      ROUND(AVG(temp_c), 1) AS t_avg,
                      MIN(temp_c) AS t_min, MAX(temp_c) AS t_max,
                      ROUND(AVG(hum_pct), 0) AS h_avg,
                      ROUND(AVG(pressure_hpa), 1) AS p_avg,
                      MIN(pressure_hpa) AS p_min, MAX(pressure_hpa) AS p_max,
                      MIN(bat_mv) AS b_min, MAX(bat_mv) AS b_max,
                      ROUND(AVG(bat_mv), 0) AS b_avg,
                      ROUND(AVG(lux), 0) AS lux_avg,
                      SUM(CASE WHEN gps_valid=1 THEN 1 ELSE 0 END) AS gps_n,
                      SUM(awake_s) AS awake_s
               FROM readings WHERE device_id IN (%s) AND seen_at >= ?
               GROUP BY day ORDER BY day DESC LIMIT ?""" \
            % ",".join("?" * len(ids))
        return [dict(r) for r in self._conn.execute(
            q, (*ids, since, days)).fetchall()]

    def prune_keep_newest(self, table: str, device_id: str, keep: int) -> int:
        """Retention helper: keep only the newest `keep` rows of `table`
        (readings|diagnostics) for one device, delete the rest. rowid-based
        so ties on seen_at can't over-delete."""
        assert table in ("readings", "diagnostics")
        with self._lock, self._conn:
            return self._conn.execute(
                f"DELETE FROM {table} WHERE device_id=? AND rowid NOT IN "
                f"(SELECT rowid FROM {table} WHERE device_id=? "
                f"ORDER BY seen_at DESC, rowid DESC LIMIT ?)",
                (device_id, device_id, keep)).rowcount

    def prune_oldest_global(self, table: str, n: int) -> int:
        """Retention helper: delete the n oldest rows of `table` across all
        devices (the DB ceiling trims oldest-first, fair to every device)."""
        assert table in ("readings", "diagnostics")
        with self._lock, self._conn:
            return self._conn.execute(
                f"DELETE FROM {table} WHERE rowid IN "
                f"(SELECT rowid FROM {table} ORDER BY seen_at ASC, rowid ASC "
                f"LIMIT ?)", (n,)).rowcount

    def vacuum(self) -> None:
        """Return freed pages to the OS after a large retention pass.
        Blocks writers for the duration — called rarely, only after big
        deletes."""
        with self._lock:
            self._conn.execute("VACUUM")

    def size_bytes(self) -> int:
        """Current on-disk size of the SQLite file."""
        with self._lock:
            return self._conn.execute("PRAGMA page_count").fetchone()[0] \
                * self._conn.execute("PRAGMA page_size").fetchone()[0]

    def backup_to(self, path: str) -> None:
        """Consistent snapshot of the DB via the sqlite3 backup API."""
        with self._lock:
            dst = sqlite3.connect(path)
            try:
                self._conn.backup(dst)
            finally:
                dst.close()


    def stats(self) -> dict:
        r = self._conn.execute(
            "SELECT COUNT(*) n, COUNT(DISTINCT device_id) d FROM readings"
        ).fetchone()
        d = self._conn.execute("SELECT COUNT(*) n FROM diagnostics").fetchone()
        return {"readings": r["n"], "devices": r["d"], "diagnostics": d["n"]}

    # ---- firmware / OTA ----

    def add_firmware(self, version: str, device_id, filename: str,
                     size: int, sha256: str) -> int:
        with self._lock, self._conn:
            cur = self._conn.execute(
                """INSERT INTO firmware (version, device_id, filename, size,
                                          sha256, active, uploaded_at)
                   VALUES (?,?,?,?,?,0,?)""",
                (version, device_id, filename, size, sha256,
                 int(time.time())))
            return cur.lastrowid

    def list_firmware(self) -> list:
        return [dict(r) for r in self._conn.execute(
            "SELECT * FROM firmware ORDER BY uploaded_at DESC").fetchall()]

    def activate_firmware(self, fw_id: int) -> bool:
        """Activate this image (and deactivate any competing one with the
        same target scope)."""
        with self._lock, self._conn:
            row = self._conn.execute(
                "SELECT device_id FROM firmware WHERE id=?", (fw_id,)
            ).fetchone()
            if not row:
                return False
            self._conn.execute(
                "UPDATE firmware SET active=0 WHERE device_id IS ? OR id=?",
                (row["device_id"], fw_id))
            self._conn.execute(
                "UPDATE firmware SET active=1 WHERE id=?", (fw_id,))
        return True

    def delete_firmware(self, fw_id: int) -> str:
        with self._lock, self._conn:
            row = self._conn.execute(
                "SELECT filename FROM firmware WHERE id=?", (fw_id,)).fetchone()
            if not row:
                return ""
            self._conn.execute("DELETE FROM firmware WHERE id=?", (fw_id,))
            return row["filename"]

    def find_update(self, device_id: str, current_version: str) -> dict:
        """Newest ACTIVE image for this device (or any), version != current."""
        rows = self._conn.execute(
            """SELECT * FROM firmware
               WHERE active=1 AND (device_id IS NULL OR device_id=?)
               ORDER BY uploaded_at DESC""",
            (device_id,)).fetchall()
        for r in rows:
            if r["version"] != current_version:
                return dict(r)
        return {}
