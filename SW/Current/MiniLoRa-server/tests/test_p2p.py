"""P2P star ingest tests (ICD §10.5): POST /p2p stores under the origin's
id, dedups on (device, frame_ts, raw_hex), maps seq → f_cnt."""

import json
import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

os.environ.setdefault("MINILORA_DB", str(
    Path(__file__).resolve().parent.parent / "data" / "test_p2p.sqlite3"))


from fastapi.testclient import TestClient          # noqa: E402
from minilora_server import config as _configmod   # noqa: E402
from minilora_server.main import app, db           # noqa: E402

# Monotonic per-test timestamps: the dedup key is (origin, frame_ts, hex)
# and the body is constant, so two lines in the same wall-clock second
# would falsely dedup across tests.
_tick = [int(time.time()) - 10000]


def _ts() -> int:
    _tick[0] += 3600
    return _tick[0]


@pytest.fixture()
def client(tmp_path, monkeypatch):
    # isolated config path (push_token empty → open like a LAN bench)
    monkeypatch.setattr(_configmod, "CONFIG_PATH_USED",
                        str(tmp_path / "config.toml"))
    return TestClient(app)


BODY = bytes(28)


def _line(origin="AABBCCDD", seq=1, ts=None, body=BODY):
    return json.dumps({"origin": origin, "seq": seq,
                       "ts": ts or _ts(), "hex": body.hex()})


def test_p2p_stores_under_origin_and_maps_seq(client):
    ts = _ts()
    r = client.post("/p2p?device=GW1", data=_line(seq=5, ts=ts) + "\n")
    assert r.status_code == 200
    j = r.json()
    assert j["ok"] is True and j["stored"] == 1
    assert j["devices"] == ["AABBCCDD"]
    rows = db.readings("AABBCCDD", limit=50)
    row = next(r for r in rows if r["frame_ts"] == ts)
    assert row["source"] == "p2p"
    assert row["f_cnt"] == 5


def test_p2p_dedups_exact_frames(client):
    line = _line(seq=6)
    client.post("/p2p", data=line + "\n")
    r = client.post("/p2p", data=line + "\n")
    assert r.json()["stored"] == 0  # same (origin, ts, hex) — already stored


def test_p2p_ignores_garbage_lines(client):
    payload = ("not json\n" + json.dumps({"origin": "XX", "hex": ""}) + "\n" +
               json.dumps({"origin": "GG", "seq": 1, "ts": int(time.time()),
                           "hex": "zz"}) + "\n" + _line(seq=7) + "\n")
    r = client.post("/p2p", data=payload)
    assert r.status_code == 200
    assert r.json()["stored"] == 1
    assert r.json()["devices"] == ["AABBCCDD"]


def test_p2p_diag_and_history_routing(client):
    now = int(time.time())
    now = _ts()
    diag = bytes([3]) + bytes(23)                      # schema 0x03, 24 B
    hist = bytes([1, 2]) + bytes(13 * 2)               # §2.1, 2 entries
    payload = (_line("AABBCCDD", 10, now, diag) + "\n" +
               _line("AABBCCDD", 11, now, hist) + "\n")
    r = client.post("/p2p", data=payload)
    assert r.status_code == 200
    # diag frame + 2 history entries (each §2.1 entry is one reading)
    assert r.json()["stored"] == 3
    assert db.has_diagnostic("AABBCCDD", now, diag.hex())


def test_p2p_kind_field_overrides_heuristic(client):
    """The batched contract: an explicit "kind" wins over the length +
    schema-byte heuristic. A 4 °C telemetry body (byte 0 = 0x04) looks
    exactly like a diag v4 schema byte to the heuristic — it must land
    as TELEMETRY when the gateway says so."""
    ts = _ts()
    body = bytearray(28)
    body[0] = 4                     # 4 °C — heuristic would say "diag v4"
    body[1] = 55                    # humidity
    line = json.dumps({"origin": "AABBCCDD", "seq": 12, "ts": ts,
                       "hex": bytes(body).hex(), "kind": "telemetry"})
    r = client.post("/p2p", data=line + "\n")
    assert r.status_code == 200 and r.json()["stored"] == 1
    rows = db.readings("AABBCCDD", limit=10)
    row = next(r for r in rows if r["frame_ts"] == ts)
    assert row["source"] == "p2p"
    assert row["temp_c"] == 4.0 and row["hum_pct"] == 55
    # ...and nothing leaked into diagnostics
    assert not db.has_diagnostic("AABBCCDD", ts, bytes(body).hex())


def test_p2p_kind_telemetry_beats_4c_heuristic_failure(client):
    """Without the kind field the OLD heuristic misfiles this frame
    (documents the bug the kind field exists to fix)."""
    ts = _ts()
    body = bytearray(28)
    body[0] = 4                     # 4 °C
    line = json.dumps({"origin": "AABBCCDD", "seq": 13, "ts": ts,
                       "hex": bytes(body).hex()})
    r = client.post("/p2p", data=line + "\n")
    assert r.status_code == 200 and r.json()["stored"] == 1
    # heuristic: routed to diagnostics (fw 0.0.0 junk row)
    assert db.has_diagnostic("AABBCCDD", ts, bytes(body).hex())


def test_p2p_rejects_oversized_batch(client):
    r = client.post("/p2p", data=b"x" * 1_100_000)
    assert r.status_code == 413
