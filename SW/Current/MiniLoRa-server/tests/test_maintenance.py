"""Device deletion and age-based pruning."""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from minilora_server.db import Database  # noqa: E402


def _tmp_db(tmp_path):
    return Database(str(tmp_path / "test.sqlite3"))


def _decoded():
    return {"gps_time": 0, "temp_c": 21.5, "hum_pct": 50, "pressure_hpa": 1013,
            "bat_mv": 4000, "lux": 10, "acc_mg": (0, 0, 100),
            "latitude": None, "longitude": None, "altitude_m": 0,
            "satellites": 0, "gps_valid": False, "awake_s": 5,
            "sleep_min": 15, "gpio_led_green": False, "gpio_button2": False}


def _seed(db, device="dev", old_offset=7200, new_offset=60):
    """One 'old' and one 'new' reading + diagnostic; old is 2 h ago."""
    now = int(time.time())
    db.insert_reading(device, "wifi", _decoded(), "aa",
                      seen_at=now - old_offset)
    db.insert_reading(device, "wifi", _decoded(), "bb",
                      seen_at=now - new_offset)
    db.insert_diagnostic(device, {
        "fw_version": "2.3.0", "boot_count": 1, "unexpected_resets": 0,
        "reset_reason": 1, "gps_timeouts": 0, "lora_join_failures": 0,
        "lora_send_failures": 0, "wifi_failures": 0, "zigbee_failures": 0,
        "sensor_read_failures": 0, "gps_consec_fails": 0, "power_flags": 0,
        "flags2": 0}, "cc", seen_at=now - old_offset)
    db.insert_diagnostic(device, {
        "fw_version": "2.3.0", "boot_count": 2, "unexpected_resets": 0,
        "reset_reason": 1, "gps_timeouts": 0, "lora_join_failures": 0,
        "lora_send_failures": 0, "wifi_failures": 0, "zigbee_failures": 0,
        "sensor_read_failures": 0, "gps_consec_fails": 0, "power_flags": 0,
        "flags2": 0}, "dd", seen_at=now - new_offset)


def test_prune_keeps_recent_and_registry(tmp_path):
    db = _tmp_db(tmp_path)
    _seed(db)
    deleted = db.prune_before("dev", 3600)  # older than 1 h
    assert deleted == 2  # old reading + old diagnostic
    ids = [d["device_id"] for d in db.devices()]
    assert "dev" in ids  # registry row survives a prune
    assert len(db.readings("dev")) == 1
    assert len(db.diagnostics("dev")) == 1


def test_prune_noop_when_nothing_old(tmp_path):
    db = _tmp_db(tmp_path)
    _seed(db, old_offset=60)
    assert db.prune_before("dev", 3600) == 0
    assert len(db.readings("dev")) == 2


def test_prune_is_scoped_to_device(tmp_path):
    db = _tmp_db(tmp_path)
    _seed(db, device="a")
    _seed(db, device="b")
    assert db.prune_before("a", 3600) == 2
    assert len(db.readings("b")) == 2  # untouched
    assert len(db.diagnostics("b")) == 2


def test_prune_all_but_last_keeps_newest_of_each(tmp_path):
    db = _tmp_db(tmp_path)
    now = int(time.time())
    for i, age in enumerate((9000, 6000, 300)):  # 3 readings, distinct ages
        db.insert_reading("dev", "wifi", _decoded(), f"r{i}",
                          seen_at=now - age)
    for i, age in enumerate((8000, 200)):        # 2 diagnostics
        db.insert_diagnostic("dev", {
            "fw_version": "2.6.0", "boot_count": i, "unexpected_resets": 0,
            "reset_reason": 1, "gps_timeouts": 0, "lora_join_failures": 0,
            "lora_send_failures": 0, "wifi_failures": 0, "zigbee_failures": 0,
            "sensor_read_failures": 0, "gps_consec_fails": 0,
            "power_flags": 0, "flags2": 0}, f"d{i}", seen_at=now - age)
    deleted = db.prune_all_but_last("dev")
    assert deleted == 3  # 2 old readings + 1 old diagnostic
    readings = db.readings("dev")
    diags = db.diagnostics("dev")
    assert len(readings) == 1 and len(diags) == 1
    assert readings[0]["raw_hex"] == "r2"  # the newest reading survives
    assert diags[0]["seen_at"] == now - 200


def test_prune_all_but_last_empty_device(tmp_path):
    db = _tmp_db(tmp_path)
    assert db.prune_all_but_last("ghost") == 0


def test_delete_device_removes_everything(tmp_path):
    db = _tmp_db(tmp_path)
    _seed(db)
    db.insert_diagnostic("dev", {  # third row for the count
        "fw_version": "2.3.0", "boot_count": 3, "unexpected_resets": 0,
        "reset_reason": 1, "gps_timeouts": 0, "lora_join_failures": 0,
        "lora_send_failures": 0, "wifi_failures": 0, "zigbee_failures": 0,
        "sensor_read_failures": 0, "gps_consec_fails": 0, "power_flags": 0,
        "flags2": 0}, "ee")
    n = db.delete_device("dev")
    assert n == 5  # 2 readings + 3 diagnostics
    assert db.devices() == []
    assert db.readings("dev") == []
    assert db.diagnostics("dev") == []


def test_delete_missing_device_is_safe(tmp_path):
    db = _tmp_db(tmp_path)
    assert db.delete_device("ghost") == 0
