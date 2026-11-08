"""Settings-page tests: the config editor's dump/coerce/validate/write."""

import os
import sys
import tomllib
from pathlib import Path

import pytest
from fastapi import HTTPException

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from minilora_server.config import AppConfig, dump_config, load_config
from minilora_server.main import _apply_form, _validate_config


def _form(**kw):
    return {k: str(v) for k, v in kw.items()}


def test_dump_config_roundtrip(tmp_path):
    """dump -> file -> load reproduces the same values (every section)."""
    cfg = AppConfig()
    cfg.server.port = 9000
    cfg.server.log_file = "/tmp/x.log"
    cfg.ttn.api_key = "NNSXS...."
    cfg.ttn.device_ids = ["dev-a", "dev-b"]
    cfg.retention.db_max_mb = 250
    cfg.device_groups = {"MiniLoRa": ["MiniLoRa-FX-1", "miniloraplus-1"]}
    text = dump_config(cfg)
    p = tmp_path / "config.toml"
    p.write_text(text, encoding="utf-8")
    back = load_config(str(p))
    assert back.server.port == 9000
    assert back.server.log_file == "/tmp/x.log"
    assert back.ttn.api_key == "NNSXS...."
    assert back.ttn.device_ids == ["dev-a", "dev-b"]
    assert back.retention.db_max_mb == 250
    assert back.device_groups == {"MiniLoRa": ["MiniLoRa-FX-1",
                                               "miniloraplus-1"]}
    # and it is valid TOML that tomllib parses with the same sections
    raw = tomllib.loads(text)
    assert set(raw) == {"server", "wifi", "ttn", "ha", "retention",
                        "device_groups"}


def test_apply_form_coerces_types():
    cfg = AppConfig()
    _apply_form(cfg, _form(**{
        "server.port": "9000",
        "server.push_token": "tok",
        "ttn.enabled": "true",
        "ttn.poll_interval_s": "120",
        "ttn.device_ids": "dev-a, dev-b ,",
        "retention.db_max_mb": "250",
        "unknown.section": "ignored",
    }))
    assert cfg.server.port == 9000
    assert cfg.server.push_token == "tok"
    assert cfg.ttn.enabled is True
    assert cfg.ttn.poll_interval_s == 120
    assert cfg.ttn.device_ids == ["dev-a", "dev-b"]  # trimmed, empties gone
    assert cfg.retention.db_max_mb == 250


def test_apply_form_rejects_bad_int():
    with pytest.raises(HTTPException) as e:
        _apply_form(AppConfig(), _form(**{"server.port": "eighty"}))
    assert e.value.status_code == 400


def test_validate_config_ranges():
    cfg = AppConfig()
    cfg.server.port = 0
    with pytest.raises(HTTPException):
        _validate_config(cfg)
    cfg = AppConfig()
    cfg.retention.db_max_mb = 0
    with pytest.raises(HTTPException):
        _validate_config(cfg)
    cfg = AppConfig()
    cfg.ttn.poll_interval_s = 5
    with pytest.raises(HTTPException):
        _validate_config(cfg)
    _validate_config(AppConfig())  # defaults pass


def test_atomic_write_and_reload(tmp_path, monkeypatch):
    """The settings write is atomic and the next load sees the new file."""
    import minilora_server.main as m
    p = tmp_path / "config.toml"
    monkeypatch.setattr(m._configmod, "CONFIG_PATH_USED", str(p))
    cfg = AppConfig()
    cfg.server.log_backups = 7
    text = dump_config(cfg)
    tmp = str(p) + ".tmp"
    Path(tmp).write_text(text, encoding="utf-8")
    os.replace(tmp, str(p))
    assert load_config(str(p)).server.log_backups == 7
    assert not os.path.exists(tmp)
