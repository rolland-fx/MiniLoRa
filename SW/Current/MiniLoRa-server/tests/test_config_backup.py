"""Config backup/restore tests: /api/config/export serves the raw file,
/api/config/import validates before writing and preserves the text."""

import sys
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from minilora_server import config as _configmod          # noqa: E402
from minilora_server.main import app                      # noqa: E402

VALID = """# a backup with comments survives the round-trip
[server]
port = 5100

[device_groups]
MiniLoRa = ["a-1", "b-2"]
"""


@pytest.fixture()
def client(tmp_path, monkeypatch):
    cfg = tmp_path / "config.toml"
    cfg.write_text(VALID, encoding="utf-8")
    monkeypatch.setattr(_configmod, "CONFIG_PATH_USED", str(cfg))
    return TestClient(app), cfg


def test_export_serves_raw_bytes_with_attachment(client):
    c, cfg = client
    r = c.get("/api/config/export")
    assert r.status_code == 200
    # raw bytes: the comment survives (not a canonicalized dump)
    assert "# a backup with comments" in r.text
    assert "port = 5100" in r.text
    assert r.headers["content-type"].startswith("application/toml")
    assert "attachment" in r.headers["content-disposition"]
    assert "minilora-config-" in r.headers["content-disposition"]


def test_export_404_without_config(tmp_path, monkeypatch):
    monkeypatch.setattr(_configmod, "CONFIG_PATH_USED",
                        str(tmp_path / "missing.toml"))
    r = TestClient(app).get("/api/config/export")
    assert r.status_code == 404


def test_import_writes_verbatim(client):
    c, cfg = client
    up = """# restored backup
[server]
port = 5100
"""
    r = c.post("/api/config/import",
               files={"file": ("config.toml", up, "application/toml")})
    assert r.status_code == 200
    j = r.json()
    assert j["imported"] is True and "restarting" not in j
    # verbatim: comment preserved, not rewritten canonically
    assert cfg.read_text(encoding="utf-8") == up


def test_import_rejects_invalid_toml(client):
    c, cfg = client
    before = cfg.read_text(encoding="utf-8")
    r = c.post("/api/config/import",
               files={"file": ("config.toml", "[server\nport = 1",
                               "application/toml")})
    assert r.status_code == 400
    assert "TOML" in r.json()["detail"]
    assert cfg.read_text(encoding="utf-8") == before  # nothing written


def test_import_rejects_out_of_range_values(client):
    c, cfg = client
    before = cfg.read_text(encoding="utf-8")
    r = c.post("/api/config/import",
               files={"file": ("config.toml",
                               "[server]\nport = 99999\n",
                               "application/toml")})
    assert r.status_code == 400
    assert cfg.read_text(encoding="utf-8") == before


def test_import_requires_file_field(client):
    r = TestClient(app).post("/api/config/import", data={"x": "1"})
    assert r.status_code == 400


def test_import_does_not_redirect_saves(tmp_path, monkeypatch):
    """Regression (live 2026-11-09): load_config() used to overwrite
    CONFIG_PATH_USED with its temp-file argument, so the SECOND import in
    a process wrote to /tmp instead of the live config."""
    boot = tmp_path / "config.toml"
    boot.write_text("[server]\nport = 5000\n", encoding="utf-8")
    monkeypatch.setattr(_configmod, "CONFIG_PATH_USED", str(boot))
    c = TestClient(app)
    up = "[server]\nport = 5100\n"
    for _ in range(2):
        r = c.post("/api/config/import",
                   files={"file": ("config.toml", up, "application/toml")})
        assert r.status_code == 200
        assert r.json()["saved"] == str(boot)     # never a temp path
    assert _configmod.CONFIG_PATH_USED == str(boot)
    assert boot.read_text(encoding="utf-8") == up
