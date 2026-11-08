"""HA bridge pure builders — no broker needed."""

import json

from minilora_server import ha
from minilora_server.config import HaConfig

CFG = HaConfig(topic_prefix="minilora", discovery_prefix="homeassistant")


def _cfg_by_suffix(configs, suffix):
    for topic, cfg in configs:
        if cfg["unique_id"].endswith(suffix):
            return topic, cfg
    raise KeyError(suffix)


def test_topics():
    assert ha.state_topic("d1", "minilora") == "minilora/device/d1/state"
    assert ha.gps_topic("d1", "minilora") == "minilora/device/d1/gps"
    assert ha.diag_topic("d1", "minilora") == "minilora/device/d1/diag"
    assert ha.availability_topic("minilora") == "minilora/server/status"
    assert ha._slug("MiniLoRa-FX-1") == "minilora_fx_1"


def test_reading_state_both_shapes():
    # decode_telemetry shape
    s = ha.reading_state(
        {"temp_c": 21.5, "hum_pct": 55, "pressure_hpa": 1030.4,
         "bat_mv": 3950, "lux": 12, "acc_mg": [10, -20, 1000],
         "latitude": 53.069, "longitude": 8.879, "altitude_m": 3,
         "satellites": 8, "gps_valid": True, "awake_s": 75,
         "sleep_min": 15},
        "wifi", 1784486251)
    assert s["temp_c"] == 21.5
    assert s["pressure_hpa"] == 1030.4
    assert s["battery_v"] == 3.95
    assert s["acc_x_g"] == 0.01 and s["acc_z_g"] == 1.0
    assert s["gps_valid"] == 1 and s["satellites"] == 8
    assert s["seen_at"] == 1784486251
    # readings-row shape (startup backfill)
    s2 = ha.reading_state(
        {"temp_c": 5.0, "hum_pct": 80, "pressure_hpa": 1000.0,
         "bat_mv": 3000, "lux": 0, "acc_x_mg": 0, "acc_y_mg": 0,
         "acc_z_mg": 0, "latitude": None, "longitude": None,
         "altitude_m": 0, "sats": 0, "gps_valid": 0,
         "awake_s": 0, "sleep_min": 0},
        "ttn-hist", 100)
    assert s2["gps_valid"] == 0 and s2["latitude"] is None
    assert s2["battery_v"] == 3.0


def test_gps_state():
    assert ha.gps_state({"latitude": None}) == {}
    assert ha.gps_state({}) == {}
    assert ha.gps_state({"latitude": 1.0, "longitude": 2.0}) == {
        "latitude": 1.0, "longitude": 2.0}


def test_discovery_configs_structure():
    configs = ha.discovery_configs("MiniLoRa-FX-1", "minilora",
                                   "homeassistant")
    topics = [t for t, _ in configs]
    # every discovery topic is unique and lives under the prefix
    assert len(topics) == len(set(topics))
    assert all(t.startswith("homeassistant/") for t in topics)
    payloads = [json.loads(json.dumps(c)) for _, c in configs]

    # tracker
    t_topic, t_cfg = _cfg_by_suffix(configs, "tracker")
    assert t_topic == ("homeassistant/device_tracker/minilora/"
                       "minilora_minilora_fx_1_tracker/config")
    assert t_cfg["source_type"] == "gps"
    assert t_cfg["state_topic"] == "minilora/device/MiniLoRa-FX-1/gps"
    assert t_cfg["json_attributes_topic"] == \
        "minilora/device/MiniLoRa-FX-1/state"
    assert t_cfg["device"]["identifiers"] == ["minilora_minilora_fx_1"]

    # headline sensors enabled by default, correct classes/units
    _, temp = _cfg_by_suffix(configs, "temperature")
    assert temp["device_class"] == "temperature"
    assert temp["unit_of_measurement"] == "°C"
    assert temp["state_class"] == "measurement"
    assert temp["value_template"] == "{{ value_json.temp_c }}"
    _, press = _cfg_by_suffix(configs, "pressure")
    assert press["device_class"] == "pressure"
    assert press["unit_of_measurement"] == "hPa"
    _, bat = _cfg_by_suffix(configs, "battery")
    assert bat["device_class"] == "voltage" and bat["unit_of_measurement"] == "V"
    _, last = _cfg_by_suffix(configs, "last_update")
    assert last["device_class"] == "timestamp"
    assert "timestamp_local" in last["value_template"]
    assert "state_class" not in last

    # diagnostics disabled by default
    _, boots = _cfg_by_suffix(configs, "boots")
    assert boots["entity_registry_enabled_default"] is False
    _, sat = _cfg_by_suffix(configs, "satellites")
    assert sat["entity_registry_enabled_default"] is False

    # every entity shares availability + unique device grouping
    for cfg in payloads:
        assert cfg["availability_topic"] == "minilora/server/status"
        assert cfg["device"]["identifiers"] == ["minilora_minilora_fx_1"]


def test_ha_config_defaults():
    cfg = HaConfig()
    assert cfg.enabled is False and cfg.port == 1883
    assert cfg.topic_prefix == "minilora"
    assert cfg.discovery_prefix == "homeassistant"
