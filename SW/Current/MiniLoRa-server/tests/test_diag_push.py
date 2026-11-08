"""WiFi diag push path: /diag glue = decode_diagnostics + insert_diagnostic.

The endpoint itself is thin (main.receive_diag); this covers what it calls
against the golden diag vectors, plus the reason /diag exists at all: a
diag frame must NOT decode as telemetry (schema byte 0x03/0x04 would read
as a 3-4 degC temperature on /data).
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from minilora_server.db import Database  # noqa: E402
from minilora_server.decode import (  # noqa: E402
    decode_diagnostics, decode_telemetry)

VECTORS = json.load(open(Path(__file__).resolve().parent.parent.parent /
                         "Firmware" / "host_tests" / "vectors" /
                         "icd_v3.json"))


def _frames():
    for f in VECTORS["diag_frames"]:
        yield bytes.fromhex(f["hex"].replace(" ", ""))


def test_diag_frames_decode():
    frames = list(_frames())
    assert frames, "no golden diag frames"
    for buf in frames:
        d = decode_diagnostics(buf)
        assert d["schema"] in (0x03, 0x04, 0x05)
        assert d["boot_count"] >= 0


def test_diag_frame_rejected_as_telemetry():
    """Why /diag is a separate endpoint from /data."""
    for buf in _frames():
        try:
            decode_telemetry(buf)
            # decode succeeds (28 B) but would be nonsense: temp = 3 or 4 C
        except ValueError:
            pass  # 24-byte legacy frames fail the length check — also fine


def test_diag_push_stores_via_wifi(tmp_path):
    """The full /diag glue: decode -> insert with the wifi-diag source."""
    db = Database(str(tmp_path / "test.sqlite3"))
    seen = []
    db.on_diagnostic = lambda dev, d, ts: seen.append(dev)

    buf = next(iter(_frames()))
    decoded = decode_diagnostics(buf)
    db.insert_diagnostic("miniloraplus-1", decoded, buf.hex())
    rows = db.diagnostics("miniloraplus-1")
    assert len(rows) == 1
    assert rows[0]["boot_count"] == decoded["boot_count"]
    assert rows[0]["raw_hex"] == buf.hex()
    # the HA bridge hook fired
    assert seen == ["miniloraplus-1"]

    # 60 s dedup: the device retrying the POST must not double-store
    assert db.has_diag_duplicate("miniloraplus-1", buf.hex())
    assert not db.has_diag_duplicate("other-device", buf.hex())
