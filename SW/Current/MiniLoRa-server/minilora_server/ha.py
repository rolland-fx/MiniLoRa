"""Home Assistant bridge: expose devices as HA entities via MQTT discovery.

The server publishes retained *discovery configs* under
``{discovery_prefix}/<component>/<node>/<object_id>/config`` and per-device
*state* JSON under ``{topic_prefix}/device/<device_id>/[state|gps|diag]``.
Home Assistant's MQTT integration auto-creates the entities; all entities of
a device are grouped under one device card via the ``device`` block.

Design notes:
- Sensor values ride ONE JSON state topic per device; each entity's
  discovery config extracts its value with a ``value_template`` — atomic
  updates, no topic sprawl.
- The GPS tracker consumes a dedicated ``gps`` topic (JSON with
  latitude/longitude) so HA's map gets position updates independently of
  the telemetry sensors.
- Diagnostics counters live on their own ``diag`` topic and are created
  disabled by default (entity_registry_enabled_default) — available when
  wanted, invisible until then.
- Availability via a retained LWT topic: entities show "unavailable" when
  the server process dies.
- History rows (fPort-4 store-and-forward) only update the tracker, never
  the sensor values (they carry no fresh telemetry).
"""

import json
import logging
import re
import threading

log = logging.getLogger("minilora.ha")

AVAILABILITY_ONLINE = "online"
AVAILABILITY_OFFLINE = "offline"


def _slug(device_id: str) -> str:
    """Device id reduced to something safe for unique_ids/entity ids."""
    return re.sub(r"[^A-Za-z0-9_]", "_", device_id).lower().strip("_")


def state_topic(device_id: str, prefix: str) -> str:
    return f"{prefix}/device/{device_id}/state"


def gps_topic(device_id: str, prefix: str) -> str:
    return f"{prefix}/device/{device_id}/gps"


def diag_topic(device_id: str, prefix: str) -> str:
    return f"{prefix}/device/{device_id}/diag"


def availability_topic(prefix: str) -> str:
    return f"{prefix}/server/status"


def reading_state(decoded: dict, source: str, seen_at: int) -> dict:
    """Canonical state JSON from a decoded telemetry payload.

    Accepts the decode_telemetry shape (acc_mg list, satellites,
    gps_valid bool) and the readings-row shape (acc_x_mg..., sats,
    gps_valid int) — the ingest paths and the startup backfill both land
    here.
    """
    acc = decoded.get("acc_mg")
    if acc is None:
        acc = [decoded.get("acc_x_mg", 0), decoded.get("acc_y_mg", 0),
               decoded.get("acc_z_mg", 0)]
    sats = decoded.get("satellites", decoded.get("sats", 0))
    return {
        "temp_c": decoded.get("temp_c", 0.0),
        "hum_pct": decoded.get("hum_pct", 0),
        "pressure_hpa": decoded.get("pressure_hpa", 0.0),
        "bat_mv": decoded.get("bat_mv", 0),
        "battery_v": (decoded.get("bat_mv", 0) or 0) / 1000.0,
        "lux": decoded.get("lux", 0),
        "acc_x_g": (acc[0] or 0) / 1000.0,
        "acc_y_g": (acc[1] or 0) / 1000.0,
        "acc_z_g": (acc[2] or 0) / 1000.0,
        "latitude": decoded.get("latitude"),
        "longitude": decoded.get("longitude"),
        "altitude_m": decoded.get("altitude_m", 0),
        "satellites": sats or 0,
        "gps_valid": 1 if decoded.get("gps_valid") else 0,
        "awake_s": decoded.get("awake_s", 0),
        "sleep_min": decoded.get("sleep_min", 0),
        "source": source,
        "seen_at": int(seen_at or 0),
    }


def gps_state(decoded: dict) -> dict:
    """Tracker payload — only published when the row carries a position."""
    lat = decoded.get("latitude")
    lon = decoded.get("longitude")
    if lat is None or lon is None:
        return {}
    return {"latitude": lat, "longitude": lon}


def _device_block(device_id: str) -> dict:
    return {
        "identifiers": [f"minilora_{_slug(device_id)}"],
        "name": device_id,
        "manufacturer": "MiniLoRa",
        "model": "MiniLoRa-FX tracker",
    }


def _entity_config(*, component: str, device_id: str, suffix: str, name: str,
                   topic: str, prefix: str, discovery_prefix: str,
                   value_template: str = None,
                   device_class: str = None, unit: str = None,
                   state_class: str = None, enabled: bool = True,
                   extra: dict = None) -> tuple:
    """Build one (discovery_topic, config_dict) pair.

    ``prefix`` is the state topic prefix, ``discovery_prefix`` the HA
    discovery base topic (usually ``homeassistant``).
    """
    slug = _slug(device_id)
    unique_id = f"minilora_{slug}_{suffix}"
    topic_path = f"{discovery_prefix}/{component}/minilora/{unique_id}/config"
    cfg = {
        "name": name,
        "unique_id": unique_id,
        "state_topic": topic,
        "availability_topic": availability_topic(prefix),
        "device": _device_block(device_id),
    }
    if value_template:
        cfg["value_template"] = value_template
    if device_class:
        cfg["device_class"] = device_class
    if unit:
        cfg["unit_of_measurement"] = unit
    if state_class:
        cfg["state_class"] = state_class
    if not enabled:
        cfg["entity_registry_enabled_default"] = False
    if extra:
        cfg.update(extra)
    return topic_path, cfg


# suffix, name, device_class, unit, state key, enabled by default
TELEMETRY_SENSORS = [
    ("temperature", "Temperature", "temperature", "°C", "temp_c", True),
    ("humidity", "Humidity", "humidity", "%", "hum_pct", True),
    ("pressure", "Pressure", "pressure", "hPa", "pressure_hpa", True),
    ("battery", "Battery", "voltage", "V", "battery_v", True),
    ("illuminance", "Illuminance", "illuminance", "lx", "lux", True),
    ("altitude", "Altitude", None, "m", "altitude_m", False),
    ("satellites", "Satellites", None, None, "satellites", False),
    ("accel_x", "Accel X", None, "g", "acc_x_g", False),
    ("accel_y", "Accel Y", None, "g", "acc_y_g", False),
    ("accel_z", "Accel Z", None, "g", "acc_z_g", False),
    ("awake", "Awake", None, "s", "awake_s", False),
    ("sleep", "Sleep", None, "min", "sleep_min", False),
    ("last_update", "Last update", "timestamp", None, "seen_at", True),
]

DIAG_SENSORS = [
    ("boots", "Boot count", "boot_count", None),
    ("unexpected_resets", "Unexpected resets", "unexpected_resets", None),
    ("last_reset_reason", "Last reset reason", "last_reset_reason", None),
    ("gps_timeouts", "GPS timeouts", "gps_timeouts", None),
    ("lora_join_failures", "LoRa join failures", "lora_join_failures", None),
    ("lora_send_failures", "LoRa send failures", "lora_send_failures", None),
    ("wifi_failures", "WiFi failures", "wifi_failures", None),
    ("gps_consec_fails", "GPS consecutive fails", "gps_consec_fails", None),
    ("last_ttff", "Last TTFF", "last_ttff", "s"),
    ("link_margin", "Link margin", "link_margin", "dB"),
    ("gw_cnt", "Gateways heard", "gw_cnt", None),
]


def discovery_configs(device_id: str, prefix: str,
                      discovery_prefix: str) -> list:
    """All discovery topics+payloads for a device (tracker + sensors + diag)."""
    st = state_topic(device_id, prefix)
    gt = gps_topic(device_id, prefix)
    dt = diag_topic(device_id, prefix)
    out = []

    # GPS tracker: state topic carries {"latitude", "longitude"}; the full
    # telemetry state rides along as attributes.
    out.append(_entity_config(
        component="device_tracker", device_id=device_id, suffix="tracker",
        name="Position", topic=gt, prefix=prefix,
        discovery_prefix=discovery_prefix,
        extra={
            "source_type": "gps",
            "json_attributes_topic": st,
        }))

    for suffix, name, dclass, unit, key, enabled in TELEMETRY_SENSORS:
        template = "{{ value_json.%s }}" % key
        if key == "seen_at":
            # device_class timestamp wants ISO 8601
            template = "{{ value_json.seen_at | timestamp_local }}"
        out.append(_entity_config(
            component="sensor", device_id=device_id, suffix=suffix,
            name=name, topic=st, prefix=prefix,
            discovery_prefix=discovery_prefix, value_template=template,
            device_class=dclass, unit=unit,
            state_class="measurement" if dclass != "timestamp" else None,
            enabled=enabled))

    for suffix, name, key, unit in DIAG_SENSORS:
        out.append(_entity_config(
            component="sensor", device_id=device_id, suffix=suffix,
            name=name, topic=dt, prefix=prefix,
            discovery_prefix=discovery_prefix,
            value_template="{{ value_json.%s }}" % key, unit=unit,
            enabled=False))

    return out


class HAPublisher:
    """MQTT client that publishes discovery + state for all devices."""

    def __init__(self, cfg):
        import paho.mqtt.client as mqtt  # lazy: only needed when enabled

        self._cfg = cfg
        self._devices_discovered = set()
        self._lock = threading.Lock()
        self._client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2, client_id=cfg.client_id)
        if cfg.username:
            self._client.username_pw_set(cfg.username, cfg.password)
        self._client.will_set(availability_topic(cfg.topic_prefix),
                              AVAILABILITY_OFFLINE, retain=True, qos=1)
        self._client.on_connect = self._on_connect

    def start(self):
        self._client.connect_async(self._cfg.broker, self._cfg.port)
        self._client.loop_start()

    def stop(self):
        try:
            self._client.publish(
                availability_topic(self._cfg.topic_prefix),
                AVAILABILITY_OFFLINE, retain=True, qos=1)
            self._client.loop_stop()
            self._client.disconnect()
        except Exception:  # never block shutdown
            log.exception("MQTT stop failed")

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if getattr(reason_code, "is_failure", False):
            log.error("MQTT connect failed: %s", reason_code)
            return
        log.info("MQTT connected to %s:%d", self._cfg.broker, self._cfg.port)
        client.publish(availability_topic(self._cfg.topic_prefix),
                       AVAILABILITY_ONLINE, retain=True, qos=1)
        # Re-announce discovery + latest states (also fires on reconnects —
        # retained configs make this idempotent).
        hook = getattr(self, "_on_connected", None)
        if hook:
            try:
                hook()
            except Exception:
                log.exception("on_connected hook failed")

    def discover_device(self, device_id: str) -> None:
        """Publish (retained) all discovery configs for a device."""
        for topic, cfg in discovery_configs(
                device_id, self._cfg.topic_prefix, self._cfg.discovery_prefix):
            self._client.publish(topic, json.dumps(cfg), retain=True, qos=1)
        with self._lock:
            self._devices_discovered.add(device_id)
        log.info("HA discovery published for %s", device_id)

    def publish_state(self, device_id: str, decoded: dict, source: str,
                      seen_at: int) -> None:
        """Telemetry sensors + tracker. History rows only move the tracker."""
        with self._lock:
            known = device_id in self._devices_discovered
        if not known:
            # First sighting of this device: send discovery before any state
            # so HA configures the entities before values arrive.
            self.discover_device(device_id)
        hist_only = source.endswith("-hist")
        gps = gps_state(decoded)
        if gps:
            self._client.publish(gps_topic(device_id, self._cfg.topic_prefix),
                                 json.dumps(gps), retain=False, qos=0)
        if hist_only:
            return
        self._client.publish(state_topic(device_id, self._cfg.topic_prefix),
                             json.dumps(reading_state(decoded, source,
                                                      seen_at)),
                             retain=False, qos=0)

    def publish_diagnostic(self, device_id: str, decoded: dict) -> None:
        self._client.publish(diag_topic(device_id, self._cfg.topic_prefix),
                             json.dumps(decoded), retain=False, qos=0)
