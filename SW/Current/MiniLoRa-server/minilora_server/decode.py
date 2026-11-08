"""MiniLoRa frame decoders — the Python mirror of the ICD (SW/Current/ICD.md).

* payload v1 telemetry, 28 bytes (frozen ABI)      — decode_telemetry()
* diagnostics schema-03, 24 bytes (fPort 2)        — decode_diagnostics()

Validated against the shared golden vectors in
../Firmware/host_tests/vectors/*.json (see tests/test_decode.py).
"""

import struct

PAYLOAD_SIZE = 28
DIAG_SIZE_V3 = 24
DIAG_SIZE_V4 = 28
# ICD 3.5 schema 0x05 byte 28: the runtime config's transport/sensor
# enables (the CFG_SET flag ids, mirroring Diagnostics.hpp kCfgFlag*).
DIAG_SIZE_V5 = DIAG_SIZE_V4 + 1
DIAG_SCHEMAS = {0x03: DIAG_SIZE_V3, 0x04: DIAG_SIZE_V4, 0x05: DIAG_SIZE_V5}
DIAG_SCHEMA = 0x05  # current

DIAG_FLAG_GPS_VALID = 0x01
DIAG_FLAG_OTA_PENDING = 0x02
DIAG_FLAG_RECOVERY = 0x04


def _u16(b: bytes, o: int) -> int:
    return b[o] | (b[o + 1] << 8)


def _i32(b: bytes, o: int) -> int:
    return struct.unpack_from("<i", b, o)[0]


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def bat_decode(raw: int) -> int:
    """Payload v1 battery byte → millivolts (± one 7.06 mV step)."""
    return round(2500 + raw * 1800 / 255)


def acc_decode(raw: int) -> float:
    """Payload v1 accel byte → LIS3DH 10-bit counts (±2048 at the
    configured ±4 g full scale — ICD §1's ±10000 map carries COUNTS,
    not milli-g: 1 g of gravity = 512 counts)."""
    return -10000 + raw * 20000 / 255


# counts → milli-g at ±4 g (4000 mg / 2048 counts). Flat on a table the
# payload reads Z ≈ +512 counts → ≈ +1000 mg; X/Y ≈ 0.
ACC_MG_PER_COUNT = 4000.0 / 2048.0


def decode_telemetry(data: bytes) -> dict:
    """Decode the frozen 28-byte payload v1 frame.

    gps_valid is a heuristic for legacy devices (zeros = no fix); v2
    firmware also sets the explicit bit in the diag frame.
    """
    if len(data) != PAYLOAD_SIZE:
        raise ValueError(f"expected {PAYLOAD_SIZE} bytes, got {len(data)}")
    temp = struct.unpack_from("<b", data, 0)[0]
    bat_mv = bat_decode(data[4])
    lat_raw = _i32(data, 10)
    lon_raw = _i32(data, 14)
    alt_raw = struct.unpack_from("<h", data, 18)[0]
    sat = data[20]
    # ICD 3.4: validity keys on the POSITION BYTES only — sats is the
    # tracked-satellite count and is populated even without a fix
    # (2026-11-03 21:02: a frame with 16 sats and a zeroed position
    # decoded as "a valid fix at 0°N 0°E" under the old triple-check).
    gps_valid = not all(v == 0 for v in data[10:18])
    return {
        "temp_c": float(temp),
        "hum_pct": data[1],
        "pressure_hpa": _u16(data, 2) / 10.0,  # payload: 0.1 hPa units
        "bat_mv": bat_mv,
        "lux": _u16(data, 5),
        "acc_mg": [
            round(acc_decode(data[7]) * ACC_MG_PER_COUNT),
            round(acc_decode(data[8]) * ACC_MG_PER_COUNT),
            round(acc_decode(data[9]) * ACC_MG_PER_COUNT),
        ],
        "gps_valid": gps_valid,
        "latitude": lat_raw / 1e6 if gps_valid else None,
        "longitude": lon_raw / 1e6 if gps_valid else None,
        "altitude_m": alt_raw - 1000,
        "satellites": sat,
        "gps_time": _u32(data, 21) or None,
        "awake_s": data[25],
        "sleep_min": data[26],
        "gpio_led_green": bool(data[27] & 0x01),
        "gpio_button2": bool(data[27] & 0x02),
    }


def decode_diagnostics(data: bytes) -> dict:
    """Decode a diagnostics frame (fPort 2). Schemas: 0x03 (24 B, legacy),
    0x04 (28 B — adds TTFF/LinkCheck/OTA status),
    0x05 (29 B — adds configFlags: the transport/sensor enables)."""
    schema = data[0] if data else 0
    if schema not in DIAG_SCHEMAS:
        raise ValueError(f"unknown diagnostics schema 0x{schema:02x}")
    if len(data) != DIAG_SCHEMAS[schema]:
        raise ValueError(f"schema 0x{schema:02x} needs {DIAG_SCHEMAS[schema]} bytes, got {len(data)}")
    flags2 = data[23]
    # ICD 3.5 schema 0x05 byte 28: the runtime config's transport/sensor
    # enables (kCfgFlag* bits). Absent (None) on pre-0x05 frames.
    config_flags = data[28] if schema >= 5 else None
    cfg = config_flags if config_flags is not None else 0
    return {
        "schema": schema,
        "fw_version": f"{data[1]}.{data[2]}.{data[3]}",
        "boot_count": _u16(data, 4),
        "unexpected_resets": _u16(data, 6),
        "reset_reason": data[8],
        "gps_timeouts": _u16(data, 9),
        "lora_join_failures": _u16(data, 11),
        "lora_send_failures": _u16(data, 13),
        "wifi_failures": _u16(data, 15),
        "zigbee_failures": _u16(data, 17),
        "sensor_read_failures": _u16(data, 19),
        "gps_consec_fails": data[21],
        "power_flags": data[22],
        "flags2": flags2,
        "power_degraded": bool(data[22] & 0x01),
        "power_critical": bool(data[22] & 0x02),
        "gps_valid": bool(flags2 & DIAG_FLAG_GPS_VALID),
        "ota_pending": bool(flags2 & DIAG_FLAG_OTA_PENDING),
        "recovery_mode": bool(flags2 & DIAG_FLAG_RECOVERY),
        "last_ttff": data[24] if schema >= 4 and data[24] != 255 else None,
        "link_margin": data[25] if schema >= 4 and data[25] != 255 else None,
        "gw_cnt": data[26] if schema >= 4 and data[26] != 255 else None,
        "ota_status": data[27] if schema >= 4 else None,
        "config_flags": config_flags,
        # Named convenience flags (None on pre-0x05 frames).
        "cfg_wifi": bool(cfg & 0x01) if config_flags is not None else None,
        "cfg_zigbee": bool(cfg & 0x02) if config_flags is not None else None,
        "cfg_ble": bool(cfg & 0x04) if config_flags is not None else None,
        "cfg_gnss": bool(cfg & 0x08) if config_flags is not None else None,
        "cfg_bme280": bool(cfg & 0x10) if config_flags is not None else None,
        "cfg_opt3001": bool(cfg & 0x20) if config_flags is not None else None,
        "cfg_lis3dh": bool(cfg & 0x40) if config_flags is not None else None,
    }


def decode_history(data: bytes) -> dict:
    """Decode the 28-byte-capable fPort-4 history frame (schema 0x01)."""
    if len(data) < 2 or data[0] != 0x01:
        raise ValueError("not a history frame")
    count = data[1]
    if count not in (1, 2) or len(data) != 2 + count * 13:
        raise ValueError("bad history frame")
    out = []
    for i in range(count):
        o = 2 + i * 13
        lat, lon, t = struct.unpack_from("<iiI", data, o)
        out.append({
            "latitude": lat / 1e6,
            "longitude": lon / 1e6,
            "gps_time": t or None,
            "satellites": data[o + 12],
        })
    return {"schema": data[0], "entries": out}


def crc16_ccitt(data: bytes) -> int:
    """CRC16-CCITT (poly 0x1021, init 0xFFFF) — ICD §4 checksum."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


_ENVELOPES = {3: 159, 4: 164}  # schema -> body length


def decode_config_envelope(frame: bytes) -> bytes:
    """Validate a BLE config write envelope; return the body.

    Schema 3 = 159-byte body (legacy), schema 4 = 164-byte body (adds the
    5 asset bytes). Raises ValueError on magic/version/length/CRC problems.
    """
    schema = frame[1] if len(frame) > 1 else 0
    body_len = _ENVELOPES.get(schema)
    if body_len is None or len(frame) != body_len + 4:
        raise ValueError(f"unsupported envelope schema/length {schema}/{len(frame)}")
    if frame[0] != 0x43:
        raise ValueError("bad magic (expected 'C')")
    body = frame[2:2 + body_len]
    crc = frame[2 + body_len] | (frame[2 + body_len + 1] << 8)
    if crc != crc16_ccitt(body):
        raise ValueError("CRC mismatch")
    return body
