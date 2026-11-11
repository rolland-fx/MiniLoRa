"""TTN webhook ingest: the webhook path must stamp received_at as
frame_ts and dedup EXACTLY (mirroring the poller path) — the old code
stored seen_at=now, so late deliveries got dishonest stamps and retries
beyond the 60-s heuristic duplicated rows.

Self-contained DB: MINILORA_DB is pointed at a dedicated test file BEFORE
minilora_server.main opens the Database at import (same pattern as
test_p2p.py — without it the module-level db lands on the LIVE
data/minilora.sqlite3 and these tests pollute/dedup against real data)."""

import base64
import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

os.environ.setdefault("MINILORA_DB", str(
    Path(__file__).resolve().parent.parent / "data" / "test_webhook.sqlite3"))


from fastapi.testclient import TestClient          # noqa: E402
from minilora_server import config as _configmod   # noqa: E402
from minilora_server.main import app, db           # noqa: E402


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setattr(_configmod, "CONFIG_PATH_USED",
                        str(tmp_path / "config.toml"))
    return TestClient(app)


def _webhook(device="dev-web", f_port=1, payload_hex=None, received_at=None):
    return {
        "end_device_ids": {"device_id": device},
        "uplink_message": {
            "f_port": f_port,
            # frm_payload is BASE64 (TTN's wire format): 28 zero bytes.
            "frm_payload": payload_hex or base64.b64encode(bytes(28)).decode(),
            "received_at": received_at or "2026-11-11T10:00:00Z",
        },
    }


def test_webhook_stamps_frame_ts(client):
    dev = "dev-web-ts"
    r = client.post("/api/ttn/webhook", json=_webhook(device=dev))
    assert r.status_code == 200 and r.json()["stored"] == 1
    rows = db.readings(dev, limit=1)
    assert rows[0]["frame_ts"] == 1794391200  # 2026-11-11T10:00:00Z
    assert rows[0]["seen_at"] == 1794391200   # the honest stamp, not "now"


def test_webhook_exact_dedup_on_retry(client):
    """The same uplink delivered twice by TTN (retry, or webhook+poller
    overlap) must store ONE row — even hours apart."""
    dev = "dev-web-dup"
    ev = _webhook(device=dev)
    r1 = client.post("/api/ttn/webhook", json=ev)
    assert r1.json()["stored"] == 1
    r2 = client.post("/api/ttn/webhook", json=ev)
    assert r2.json()["stored"] == 0
    r3 = client.post("/api/ttn/webhook", json=ev)
    assert r3.json()["stored"] == 0
    assert len(db.readings(dev, limit=10)) == 1


def test_webhook_diag_exact_dedup(client):
    dev = "dev-web-diag"
    diag = bytes([5]) + bytes(28)   # schema 0x05, 29 B
    ev = _webhook(device=dev, f_port=2,
                  payload_hex=base64.b64encode(diag).decode())
    r1 = client.post("/api/ttn/webhook", json=ev)
    assert r1.json()["stored"] == 1
    r2 = client.post("/api/ttn/webhook", json=ev)
    assert r2.json()["stored"] == 0
    assert len(db.diagnostics(dev, limit=10)) == 1


def test_webhook_without_received_at_still_stores(client):
    """Legacy webhook configs that omit received_at: fall back to the
    60-s heuristic path (seen_at=now), never drop the frame."""
    dev = "dev-web-nora"
    ev = _webhook(device=dev)
    ev["uplink_message"]["received_at"] = ""
    r = client.post("/api/ttn/webhook", json=ev)
    assert r.status_code == 200 and r.json()["stored"] == 1
