"""TTN poller dedup: exact-uplink keys, history fix keys, NDJSON parsing."""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from minilora_server.db import Database  # noqa: E402
from minilora_server.ttn import (  # noqa: E402
    parse_uplink_event, received_at_to_epoch)


def _tmp_db(tmp_path):
    return Database(str(tmp_path / "test.sqlite3"))


def test_received_at_parsing():
    assert received_at_to_epoch(
        "2026-11-01T15:50:20.123456789Z") == received_at_to_epoch(
        "2026-11-01T15:50:20.123Z")
    assert received_at_to_epoch("2026-11-01T15:50:20Z") is not None
    assert received_at_to_epoch("") is None
    assert received_at_to_epoch("garbage") is None


def test_parse_uplink_event_shapes():
    webhook = {"end_device_ids": {"device_id": "dev-1"},
               "uplink_message": {"f_port": 1,
                                  "frm_payload": "CgAAAAAAAAAAAAAAAAAAAHMA",
                                  "received_at": "2026-11-01T15:00:00Z"}}
    evs = parse_uplink_event(webhook)
    assert len(evs) == 1 and evs[0]["device_id"] == "dev-1"
    assert evs[0]["received_at"] == "2026-11-01T15:00:00Z"
    # storage-API NDJSON line shape
    evs2 = parse_uplink_event({"result": webhook})
    assert evs2 == evs


def test_exact_frame_dedup(tmp_path):
    db = _tmp_db(tmp_path)
    decoded = {"temp_c": 20.0, "hum_pct": 50, "pressure_hpa": 1013.0,
               "bat_mv": 4000, "lux": 0, "acc_mg": [0, 0, 0],
               "latitude": 0.0, "longitude": 0.0, "altitude_m": 0,
               "satellites": 0, "gps_valid": False, "gps_time": None,
               "awake_s": 5, "sleep_min": 5, "gpio_led_green": False,
               "gpio_button2": False}
    # frame 1 stored at t0
    db.insert_reading("dev", "ttn", decoded, "aabb", frame_ts=1_000_000)
    assert db.has_frame("dev", "ttn", 1_000_000, "aabb")
    # same frame re-seen much later (simulated 12 h re-poll) -> dedup
    assert db.has_frame("dev", "ttn", 1_000_000, "aabb")
    # a genuinely different uplink (different received_at) is not deduped
    assert not db.has_frame("dev", "ttn", 1_000_060, "aabb")
    # identical bytes but different received_at: kept (legit new frame)
    db.insert_reading("dev", "ttn", decoded, "aabb", frame_ts=1_000_060)
    assert not db.has_frame("dev", "ttn", 1_000_120, "aabb")


def test_history_fix_dedup(tmp_path):
    db = _tmp_db(tmp_path)
    decoded = {"temp_c": 0.0, "hum_pct": 0, "pressure_hpa": 0, "bat_mv": 0,
               "lux": 0, "acc_mg": [0, 0, 0], "latitude": 53.0,
               "longitude": 8.8, "altitude_m": 0, "satellites": 9,
               "gps_valid": True, "gps_time": 1_700_000_000,
               "awake_s": 0, "sleep_min": 0, "gpio_led_green": False,
               "gpio_button2": False}
    db.insert_reading("dev", "ttn-hist", decoded, "ff" * 28)
    assert db.has_history_fix("dev", 1_700_000_000)
    assert not db.has_history_fix("dev", 1_700_000_060)


def test_diag_exact_dedup(tmp_path):
    """Diag frames dedup against the diagnostics table (they are stored
    there, not in readings — the wrong-table check duplicated every
    polled diag frame on each 5-min poll)."""
    db = _tmp_db(tmp_path)
    decoded = {"fw_version": "2.6.0", "boot_count": 192,
               "unexpected_resets": 0, "reset_reason": 1,
               "gps_timeouts": 15, "lora_join_failures": 6,
               "lora_send_failures": 0, "wifi_failures": 4,
               "zigbee_failures": 0, "sensor_read_failures": 0,
               "gps_consec_fails": 15, "power_flags": 0, "flags2": 0}
    db.insert_diagnostic("dev", decoded, "aabb", seen_at=1_000_000)
    assert db.has_diagnostic("dev", 1_000_000, "aabb")
    # same frame re-polled hours later -> deduped
    assert db.has_diagnostic("dev", 1_000_000, "aabb")
    # a genuinely different diag frame is kept
    assert not db.has_diagnostic("dev", 1_000_300, "aabb")
    db.insert_diagnostic("dev", decoded, "ccdd", seen_at=1_000_300)
    assert len(db.diagnostics("dev")) == 2


def test_legacy_60s_window_still_works(tmp_path):
    db = _tmp_db(tmp_path)
    decoded = {"temp_c": 0.0, "hum_pct": 0, "pressure_hpa": 0, "bat_mv": 0,
               "lux": 0, "acc_mg": [0, 0, 0], "latitude": 0.0,
               "longitude": 0.0, "altitude_m": 0, "satellites": 0,
               "gps_valid": False, "gps_time": None, "awake_s": 0,
               "sleep_min": 0, "gpio_led_green": False,
               "gpio_button2": False}
    db.insert_reading("dev", "ttn", decoded, "cc", seen_at=int(time.time()))
    assert db.has_duplicate("dev", "ttn", "cc")
