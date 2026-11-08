"""Decoder tests against the shared golden vectors (payload v1 + diag v3)
and the config-envelope CRC path."""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent
                       / "Firmware"))

from minilora_server.decode import (  # noqa: E402
    decode_config_envelope, decode_diagnostics, decode_telemetry,
)

VEC = (Path(__file__).resolve().parent.parent.parent / "Firmware"
       / "host_tests" / "vectors")


def _frames(name):
    return json.loads((VEC / name).read_text())


def test_telemetry_golden_vectors():
    for v in _frames("payload_v1.json")["vectors"]:
        buf = bytes.fromhex(v["hex"].replace(" ", ""))
        d = decode_telemetry(buf)
        e = v["expected"]
        assert d["temp_c"] == e["temp"], v["name"]
        assert d["hum_pct"] == e["hum"], v["name"]
        assert d["pressure_hpa"] == e["pres"] / 10, v["name"]
        # battery quantizes to 255 steps (~7 mV); saturating vectors clamp
        bat_exp = e["bat_mv"]
        if v.get("saturating"):
            bat_exp = min(4300, max(2500, bat_exp))
        assert abs(d["bat_mv"] - bat_exp) <= 8, (v["name"], d["bat_mv"], bat_exp)
        assert d["latitude"] == (e["lat"] / 1e6 if e["lat"] else None), v["name"]
        assert d["longitude"] == (e["lon"] / 1e6 if e["lon"] else None), v["name"]
        assert d["altitude_m"] == e["alt"], v["name"]
        assert d["satellites"] == e["sat"], v["name"]
        assert d["gps_time"] == (e["time"] or None), v["name"]
        assert d["gps_valid"] == (not (e["lat"] == 0 and e["lon"] == 0
                                        and e["sat"] == 0)), v["name"]


def test_diagnostics_golden_vectors():
    for f in _frames("icd_v3.json")["diag_frames"]:
        buf = bytes.fromhex(f["hex"].replace(" ", ""))
        d = decode_diagnostics(buf)
        e = f["expected"]
        assert d["schema"] == e.get("schema", 4), f["name"]
        if "configFlags" in e:  # schema 0x05: transport/sensor enables
            assert d.get("config_flags") == e["configFlags"], f["name"]
        assert d["fw_version"] == ".".join(map(str, e["fw"])), f["name"]
        assert d["boot_count"] == e["bootCount"]
        assert d["unexpected_resets"] == e["unexpectedResets"]
        assert d["gps_timeouts"] == e["gpsTimeouts"]
        assert d["gps_consec_fails"] == e["gpsConsecFails"]
        assert d["power_flags"] == e["powerFlags"]
        assert d["recovery_mode"] == bool(e["flags2"] & 0x04), f["name"]


def test_config_envelope_crc():
    icd = _frames("icd_v3.json")
    good = bytes.fromhex(
        icd["config_envelope"][0]["hex"].replace(" ", ""))
    body = decode_config_envelope(good)
    assert len(body) == 159 and body[0] == 0

    bad = bytes.fromhex(
        icd["config_envelope"][1]["hex"].replace(" ", ""))
    try:
        decode_config_envelope(bad)
        raise AssertionError("bad CRC must raise")
    except ValueError as e:
        assert "CRC" in str(e)


def test_bad_lengths_raise():
    import pytest
    with pytest.raises(ValueError):
        decode_telemetry(bytes(27))
    with pytest.raises(ValueError):
        decode_diagnostics(bytes(23))
    with pytest.raises(ValueError):
        decode_diagnostics(bytes(24))  # schema 0x00, not 0x03
