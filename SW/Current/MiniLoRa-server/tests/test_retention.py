"""Retention + size-capped logging tests (deployment guarantees)."""

import logging
import logging.handlers
import os
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from minilora_server.config import AppConfig, RetentionConfig, ServerConfig
from minilora_server.db import Database

RCOLS = ('device_id, source, seen_at, frame_ts, gps_time, temp_c, hum_pct, '
         'pressure_hpa, bat_mv, lux, acc_x_mg, acc_y_mg, acc_z_mg, latitude, '
         'longitude, altitude_m, sats, gps_valid, awake_s, sleep_min, gpio, '
         'raw_hex')


def _mkdb(tmp_path, n=500, dev="dev-1"):
    db = Database(str(tmp_path / "t.sqlite3"))
    now = int(time.time())
    rows = []
    for i in range(n):
        rows.append((dev, 'ttn', now - n + i, now - n + i, now - n + i,
                     21.5, 55.0, 1013.2, 3950, 120, 33, -12, 544,
                     48.85, 2.35, 35, 9, 1, 6, 15, 0, '%056X' % i))
    ph = ','.join('?' * 22)
    with db._lock, db._conn:
        db._conn.executemany(
            f"INSERT INTO readings ({RCOLS}) VALUES ({ph})", rows)
        db._conn.executemany(
            "INSERT INTO diagnostics (device_id, seen_at, fw_version, "
            "boot_count, raw_hex) VALUES (?, ?, '2.6.16', ?, ?)",
            [(dev, now - i, i, '%058X' % i) for i in range(n // 16)])
        db._conn.commit()
    return db, now


def test_prune_keep_newest_keeps_newest_n(tmp_path):
    db, now = _mkdb(tmp_path, n=500)
    deleted = db.prune_keep_newest("readings", "dev-1", 100)
    assert deleted == 400
    left = [r["seen_at"] for r in db.readings("dev-1", limit=1000)]
    assert len(left) == 100
    assert left == sorted(left, reverse=True)      # newest kept
    assert left[0] == now - 1                      # the very newest survived
    # diagnostics untouched by the readings pass
    n_diag = db._conn.execute(
        "SELECT COUNT(*) FROM diagnostics WHERE device_id='dev-1'").fetchone()[0]
    assert n_diag == 500 // 16
    # and the diag cap works the same way
    assert db.prune_keep_newest("diagnostics", "dev-1", 10) == 500 // 16 - 10


def test_prune_oldest_global_is_oldest_first(tmp_path):
    db, now = _mkdb(tmp_path, n=300)
    assert db.prune_oldest_global("readings", 50) == 50
    oldest_left = db._conn.execute(
        "SELECT MIN(seen_at) FROM readings").fetchone()[0]
    assert oldest_left == now - 300 + 50  # exactly the first 50 gone


def test_size_bytes_and_vacuum(tmp_path):
    db, _ = _mkdb(tmp_path, n=2000)
    size_before = db.size_bytes()
    assert size_before > 0
    db.prune_keep_newest("readings", "dev-1", 10)
    db.vacuum()
    assert db.size_bytes() < size_before  # pages actually handed back


def test_retention_config_defaults_and_env(monkeypatch):
    cfg = AppConfig()
    assert cfg.retention.enabled is True
    assert cfg.retention.db_max_mb == 100
    assert cfg.server.log_file == ""
    monkeypatch.setenv("MINILORA_LOG_FILE", "/tmp/x.log")
    monkeypatch.setenv("MINILORA_LOG_MAX_MB", "9")
    from minilora_server.config import load_config
    c = load_config("/nonexistent/path.toml")
    assert c.server.log_file == "/tmp/x.log"
    assert c.server.log_max_mb == 9


def test_enforce_retention_enforces_the_ceiling(tmp_path, monkeypatch):
    """End-to-end: a DB over db_max_mb gets trimmed oldest-first and the
    vacuum hands the pages back (a 25k-row DB vs a 1 MB cap clears it —
    the blunt-instrument edge; the newest-survives property is covered by
    test_prune_keep_newest per device)."""
    import minilora_server.main as m
    db, now = _mkdb(tmp_path, n=25_000)
    n_diag = 25_000 // 16
    monkeypatch.setattr(m, "db", db)
    monkeypatch.setattr(m, "cfg",
                        AppConfig(retention=RetentionConfig(
                            enabled=True, readings_per_device=2_000_000,
                            diagnostics_per_device=2_000_000,
                            db_max_mb=1)))  # 1 MB vs ~5 MB of rows
    out = m.enforce_retention()
    deleted = out["global"]["readings"]
    assert 0 < deleted <= 25_000
    assert "vacuumed" in out                       # >20k deleted: reclaim
    assert db.size_bytes() < 1_500_000             # pages handed back
    assert db._conn.execute("SELECT COUNT(*) FROM readings").fetchone()[0] \
        == 25_000 - deleted
    # oldest-first: the oldest surviving row is exactly `deleted` in
    assert db._conn.execute("SELECT MIN(seen_at) FROM readings").fetchone()[0] \
        == now - 25_000 + deleted
    assert db._conn.execute("SELECT COUNT(*) FROM diagnostics").fetchone()[0] \
        == n_diag - out["global"]["diagnostics"]
