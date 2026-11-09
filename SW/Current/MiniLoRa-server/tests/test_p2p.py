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

# Fresh DB per pytest run: the module-level Database() opens the env path
# at import — deleting here gives every run a clean slate (the file is a
# test artifact, never the live DB).
_p2p_db = os.environ["MINILORA_DB"]
if os.path.exists(_p2p_db):
    os.remove(_p2p_db)

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
