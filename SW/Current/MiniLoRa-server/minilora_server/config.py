"""MiniLoRa-server configuration (TOML, stdlib tomllib)."""

import tomllib
from dataclasses import dataclass, field, fields as _dc_fields
from pathlib import Path
from typing import Dict, List, Optional


@dataclass
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 8080
    db_path: str = "data/minilora.sqlite3"
    # Optional shared token for the WiFi push endpoint (?token= or X-MiniLoRa-Token)
    push_token: str = ""
    # Log file with size-capped rotation (empty = stderr only, the default).
    # The rotation keeps the file forever bounded: log_max_mb + backups.
    log_file: str = ""
    log_max_mb: int = 5
    log_backups: int = 3


@dataclass
class RetentionConfig:
    """Keeps the SQLite file bounded for set-and-forget LAN deployments.

    Readings cost ~200 B/row on disk (measured, indexes included), so the
    defaults sit far under the 100 MB target: 200k readings/device ≈ 40 MB.
    """
    enabled: bool = True
    readings_per_device: int = 200_000
    diagnostics_per_device: int = 50_000
    db_max_mb: int = 100              # hard ceiling; global oldest-first trim
    check_interval_s: int = 3600      # hourly enforcement


@dataclass
class WifiConfig:
    # How un-identified device pushes are labelled when no ?device= is given
    default_device: str = "wifi-device"


@dataclass
class TTNConfig:
    enabled: bool = False
    # Webhook: TTN pushes to POST /api/ttn/webhook?token=...
    webhook_token: str = ""
    # Poller: pull the storage API on an interval (same API as the phone app)
    region: str = "eu1"
    app_id: str = ""
    api_key: str = ""
    device_ids: List[str] = field(default_factory=list)
    poll_interval_s: int = 300
    poll_limit: int = 20      # app-proven value (TTNClient uses the same)
    poll_window_h: int = 12   # free-tier storage keeps 24 h — 12 h re-fetch
                              # with exact dedup can't miss anything


@dataclass
class HaConfig:
    """Home Assistant bridge via MQTT discovery (needs an MQTT broker)."""
    enabled: bool = False
    broker: str = ""
    port: int = 1883
    username: str = ""
    password: str = ""
    topic_prefix: str = "minilora"          # state: {prefix}/device/<name>/...
    discovery_prefix: str = "homeassistant"  # HA's discovery base topic
    client_id: str = "minilora-server"


@dataclass
class AppConfig:
    server: ServerConfig = field(default_factory=ServerConfig)
    wifi: WifiConfig = field(default_factory=WifiConfig)
    ttn: TTNConfig = field(default_factory=TTNConfig)
    ha: HaConfig = field(default_factory=HaConfig)
    retention: RetentionConfig = field(default_factory=RetentionConfig)
    # Optional physical-device grouping for the dashboard: one tracker that
    # reports under several identities (WiFi push name + TTN device id) is
    # ONE row with transport badges instead of two confusing lookalikes.
    #   [device_groups]
    #   MiniLoRa = ["MiniLoRa-FX-1", "miniloraplus-1"]
    # Purely presentational — the DB keys stay per-identity.
    device_groups: Dict[str, List[str]] = field(default_factory=dict)


# Path of the config file load_config() actually used — the settings page
# writes edits back to it. Empty string = no file was found; the settings
# page then falls back to ./config.toml.
CONFIG_PATH_USED = ""


def dump_config(cfg: AppConfig) -> str:
    """Serialize the config back to TOML for the settings page.

    Comments from the original file are lost (the UI says so); env
    overrides (MINILORA_*) still win at load time.
    """
    import json as _json

    def fmt(v):
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, str):
            return _json.dumps(v)      # a TOML basic string for plain values
        if isinstance(v, list):
            return "[" + ", ".join(_json.dumps(x) for x in v) + "]"
        return str(v)

    lines = []
    for name, obj in (("server", cfg.server), ("wifi", cfg.wifi),
                      ("ttn", cfg.ttn), ("ha", cfg.ha),
                      ("retention", cfg.retention)):
        lines.append(f"[{name}]")
        for f in _dc_fields(obj):
            lines.append(f"{f.name} = {fmt(getattr(obj, f.name))}")
        lines.append("")
    if cfg.device_groups:
        lines.append("[device_groups]")
        for k, ids in cfg.device_groups.items():
            lines.append(f"{_json.dumps(k)} = {fmt(list(ids))}")
        lines.append("")
    return "\n".join(lines) + "\n"


def load_config(path: Optional[str] = None, record: bool = True) -> AppConfig:
    """Load config.toml (missing keys/sections fall back to defaults).

    Env overrides: MINILORA_CONFIG (alternate file), MINILORA_DB (db path) —
    used by the Docker image to point at the mounted volume.

    record: write the resolved path to CONFIG_PATH_USED — the "config this
    server booted with" marker that /api/config saves back to. Validation
    side-loads (the config import) must pass record=False or they would
    redirect every later save to their temp file (found live 2026-11-09:
    a restore wrote its config to /tmp because the boot marker had been
    overwritten by the first import's validation load).
    """
    import os

    cfg = AppConfig()
    if path is None:
        for candidate in ("config.toml", "config.example.toml"):
            p = Path(__file__).resolve().parent.parent / candidate
            if p.exists():
                path = str(p)
                break
    env_cfg = os.environ.get("MINILORA_CONFIG")
    if env_cfg:
        path = env_cfg
    if path and Path(path).exists():
        raw = tomllib.loads(Path(path).read_text(encoding="utf-8"))
        if "server" in raw:
            for k, v in raw["server"].items():
                if hasattr(cfg.server, k):
                    setattr(cfg.server, k, type(getattr(cfg.server, k))(v))
        if "wifi" in raw:
            for k, v in raw["wifi"].items():
                if hasattr(cfg.wifi, k):
                    setattr(cfg.wifi, k, type(getattr(cfg.wifi, k))(v))
        if "ttn" in raw:
            for k, v in raw["ttn"].items():
                if hasattr(cfg.ttn, k):
                    if k == "device_ids":
                        cfg.ttn.device_ids = list(v)
                    else:
                        setattr(cfg.ttn, k, type(getattr(cfg.ttn, k))(v))
        if "ha" in raw:
            for k, v in raw["ha"].items():
                if hasattr(cfg.ha, k):
                    setattr(cfg.ha, k, type(getattr(cfg.ha, k))(v))
        if "retention" in raw:
            for k, v in raw["retention"].items():
                if hasattr(cfg.retention, k):
                    setattr(cfg.retention, k,
                            type(getattr(cfg.retention, k))(v))
        if "device_groups" in raw:
            for k, v in raw["device_groups"].items():
                cfg.device_groups[str(k)] = [str(x) for x in v]

    if os.environ.get("MINILORA_DB"):
        cfg.server.db_path = os.environ["MINILORA_DB"]
    # Bind overrides for containers: the image always serves on 8080 no
    # matter what the mounted config's [server] says.
    if os.environ.get("MINILORA_HOST"):
        cfg.server.host = os.environ["MINILORA_HOST"]
    if os.environ.get("MINILORA_PORT"):
        cfg.server.port = int(os.environ["MINILORA_PORT"])
    if os.environ.get("MINILORA_LOG_FILE"):
        cfg.server.log_file = os.environ["MINILORA_LOG_FILE"]
    if os.environ.get("MINILORA_LOG_MAX_MB"):
        cfg.server.log_max_mb = int(os.environ["MINILORA_LOG_MAX_MB"])
    if os.environ.get("MINILORA_LOG_BACKUPS"):
        cfg.server.log_backups = int(os.environ["MINILORA_LOG_BACKUPS"])

    if record:
        global CONFIG_PATH_USED
        CONFIG_PATH_USED = path or ""
    return cfg
