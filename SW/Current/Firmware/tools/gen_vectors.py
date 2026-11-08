#!/usr/bin/env python3
"""
Golden vector generator for the MiniLoRa ICD:

  - payload v1 uplink (28 bytes, FROZEN)      -> payload_v1.json + vectors.hpp
  - diagnostics v3 uplink (24 bytes, fport 2) -> icd_v3.json
  - config write v3 envelope (162 B, BLE)     -> icd_v3.json
  - downlink command frames (fport 3)         -> icd_v3.json

Single source of truth for:
  - host_tests/vectors/vectors.hpp   (C++ — v2 firmware host unit tests)
  - host_tests/vectors/payload_v1.json (JSON — TTN decoder.js, Android, Flask server)
  - host_tests/vectors/icd_v3.json   (JSON — TTN decoder v2, Android app v2)

Encoding rules (must match MiniLoRa.ino `buildUplinkPayload`, tag Before-Rework):
  byte 0      temp      int8   °C, direct
  byte 1      hum       uint8  %RH, direct
  bytes 2-3   pressure  uint16 0.1 hPa (deci-hPa), little-endian
  byte 4      battery   uint8  ((mv - 2500) * 255) // 1800   (v1 truncating map)
  bytes 5-6   lux       uint16 little-endian
  bytes 7-9   acc       uint8  (raw + 10000) * 255 // 20000 per axis (v1 map)
  bytes 10-13 lat       int32  microdegrees, little-endian
  bytes 14-17 lon       int32  microdegrees, little-endian
  bytes 18-19 altitude  int16  meters + 1000, little-endian
  byte  20    satellites uint8
  bytes 21-24 time      uint32 unix epoch s, little-endian
  byte  25    awake_sec uint8
  byte  26    sleep_min uint8
  byte  27    gpio      uint8  bit0 = GPIO6/LED_GREEN, bit1 = Button_2

v2 divergence (documented in PHASE2_DOMAIN.md): v2 saturates battery and
accelerometer maps at the range edges; v1 wraps (integer overflow truncation).
Vectors that exercise saturation are flagged "saturating": v1 firmware and the
v2 firmware intentionally differ there.

Diagnostics v4 frame (28 bytes, fport 2 — see ICD.md):
  byte  0      schema = 0x04 (decoders switch on THIS, forever)
  bytes 1-3    fw version major, minor, patch
  bytes 4-5    bootCount            uint16 LE
  bytes 6-7    unexpectedResets     uint16 LE
  byte  8      lastResetReason      uint8 (esp_reset_reason_t)
  bytes 9-10   gpsTimeouts          uint16 LE
  bytes 11-12  loraJoinFailures     uint16 LE
  bytes 13-14  loraSendFailures     uint16 LE
  bytes 15-16  wifiFailures         uint16 LE
  bytes 17-18  zigbeeFailures       uint16 LE
  bytes 19-20  sensorReadFailures   uint16 LE
  byte  21     gpsConsecFails       uint8
  byte  22     powerFlags           bit0 degraded, bit1 critical
  byte  23     flags2               bit0 gpsValid, bit1 otaPendingVerify,
                                    bit2 recoveryMode
  byte  24     lastTtff             uint8, s (255 = unknown)
  byte  25     linkMargin           uint8, dB (255 = unknown)
  byte  26     gwCnt                uint8 (255 = unknown)
  byte  27     otaStatus            uint8 (kOtaStatus*)

Diagnostics v5 frame (29 bytes — ICD 3.5): adds
  byte  28     configFlags          uint8 — runtime transport/sensor enables
                                     (kCfgFlag* bits: bit0 WiFi, bit1 Zigbee,
                                     bit2 BLE beacon, bit3 GNSS, bit4 BME280,
                                     bit5 OPT3001, bit6 LIS3DH)

Config write v3 envelope (BLE, 163 bytes):
  byte 0      magic 'C' (0x43)
  byte 1      schema version = 3
  bytes 2-160 DeviceConfig body (159 bytes: legacy v1 158 + cfgZigbee)
  bytes 161-162 CRC16-CCITT (poly 0x1021, init 0xFFFF) of bytes 2..160, LE

Downlink command frame (fport 3): [0x4D][opcode][arg]
  0x01 locate_now, 0x02 send_diag, 0x03 reset_diag, 0x04 ble_window
"""

import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "host_tests" / "vectors"


def map_u8(v: int, lo: int, hi: int, saturate: bool = False) -> int:
    """v1 Arduino-map semantics ((v-lo)*255//(hi-lo)), optionally saturated (v2)."""
    if saturate:
        v = max(lo, min(hi, v))
    return ((v - lo) * 255) // (hi - lo)


def bat_encode(mv: int, saturate=False) -> int:
    return map_u8(mv, 2500, 4300, saturate)


def acc_encode(raw: int, saturate=False) -> int:
    return map_u8(raw, -10000, 10000, saturate)


def encode(t: dict, saturate: bool = False) -> bytes:
    """Canonical v1 encoder; v2 saturating mode for vectors flagged 'saturating'."""
    return struct.pack(
        "<bB H B H 3B i i h B I B B B",
        t["temp"], t["hum"], t["pres"],
        bat_encode(t["bat_mv"], saturate),
        t["lux"],
        *(acc_encode(a, saturate) for a in t["acc"]),
        t["lat"], t["lon"], t["alt"] + 1000,
        t["sat"], t["time"], t["awake"], t["sleep"], t["gpio"],
    )


def bat_decode(b: int) -> int:
    return round(2500 + b * 1800 / 255)


def acc_decode(b: int) -> int:
    return round(-10000 + b * 20000 / 255)


def decode(buf: bytes) -> dict:
    (temp, hum, pres, bat_b, lux, ax, ay, az,
     lat, lon, alt_raw, sat, tsec, awake, sleep, gpio) = struct.unpack(
        "<bB H B H 3B i i h B I B B B", buf)
    return {
        "temp": temp, "hum": hum, "pres": pres,
        "bat_mv": bat_decode(bat_b), "bat_raw": bat_b,
        "lux": lux,
        "acc_raw": [acc_decode(ax), acc_decode(ay), acc_decode(az)],
        "acc_raw_bytes": [ax, ay, az],
        "lat": lat / 1_000_000, "lon": lon / 1_000_000,
        "alt": alt_raw - 1000, "sat": sat,
        "time": tsec, "awake": awake, "sleep": sleep, "gpio": gpio,
    }


VECTORS = [
    {
        "name": "doc_example",
        "description": "The worked example from payload_format.md (hex is the contract; "
                       "the decoded table below is recomputed authoritatively).",
        "telemetry": {
            "temp": 26, "hum": 15, "pres": 10170, "bat_mv": 4231,
            "lux": 1071, "acc": [0, 0, 0],
            "lat": 53069522, "lon": 8879780,  # microdegrees
            "alt": -3, "sat": 8, "time": 1784486251,
            "awake": 75, "sleep": 15, "gpio": 1,
        },
    },
    {
        "name": "negative_coords_night",
        "description": "Southern/western hemisphere, cold night, negative altitude.",
        "telemetry": {
            "temp": -12, "hum": 88, "pres": 9950, "bat_mv": 3700,
            "lux": 0, "acc": [-2500, 3200, -9800],
            "lat": -45123456, "lon": -123456789,
            "alt": -350, "sat": 5, "time": 1760000000,
            "awake": 42, "sleep": 60, "gpio": 0,
        },
    },
    {
        "name": "no_fix",
        "description": "GNSS enabled but no fix: lat/lon/time are zero, sat=0. "
                       "v2 sets diag flags elsewhere; the frame itself is unchanged.",
        "telemetry": {
            "temp": 21, "hum": 55, "pres": 10130, "bat_mv": 3980,
            "lux": 250, "acc": [12, -7, 4096],
            "lat": 0, "lon": 0, "alt": 0, "sat": 0, "time": 0,
            "awake": 150, "sleep": 10, "gpio": 0,
        },
    },
    {
        "name": "edges",
        "description": "In-range extremes: battery and accelerometer exactly at map edges.",
        "telemetry": {
            "temp": -40, "hum": 100, "pres": 11000, "bat_mv": 4300,
            "lux": 65535, "acc": [-10000, 10000, 0],
            "lat": 89999999, "lon": -179999999,
            "alt": 8848, "sat": 255, "time": 4294967295,
            "awake": 255, "sleep": 120, "gpio": 3,
        },
    },
    {
        "name": "deep_winter_long_sleep",
        "description": "Max sleep index from SLEEP_T (120 min), gpio bits set.",
        "telemetry": {
            "temp": -25, "hum": 71, "pres": 10420, "bat_mv": 2500,
            "lux": 3, "acc": [64, -64, 8000],
            "lat": 47842310, "lon": 231000,
            "alt": 102, "sat": 9, "time": 1767225600,
            "awake": 200, "sleep": 120, "gpio": 3,
        },
    },
    {
        "name": "acc_saturation",
        "description": "Saturating: raw 15000 exceeds the ±10000 map range. "
                       "v2 clamps to 255; v1 firmware would wrap to 62. Flagged divergent.",
        "telemetry": {
            "temp": 30, "hum": 40, "pres": 10000, "bat_mv": 4500,
            "lux": 10, "acc": [15000, -15000, 0],
            "lat": 0, "lon": 0, "alt": 0, "sat": 0, "time": 100,
            "awake": 10, "sleep": 1, "gpio": 0,
        },
        "saturating": True,
    },
]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    vectors = []
    for v in VECTORS:
        buf = encode(v["telemetry"], saturate=v.get("saturating", False))
        assert len(buf) == 28, len(buf)
        vectors.append({
            "name": v["name"],
            "description": v["description"],
            "saturating": v.get("saturating", False),
            "hex": buf.hex(" ").upper(),
            "expected": v["telemetry"],
            "decoded": decode(buf),
        })

    (OUT / "payload_v1.json").write_text(
        json.dumps({"format": "payload_v1", "length": 28, "vectors": vectors},
                   indent=2) + "\n")

    # ---- ICD v3 vectors: diag frames, config envelope, downlink commands ----
    icd = {
        "format": "icd_v3",
        "diag_schema": 5,
        "diag_length": 29,
        "diag_frames": [],
        "config_envelope": [],
        "downlink_commands": [],
    }

    diag_frames = [
        {
            "name": "diag_healthy",
            "description": "Healthy device: fw 2.5.0, 1234 boots, no failures, "
                           "GPS valid, OTA confirmed, TTFF 9 s, heard by 3 "
                           "gateways at 12 dB margin. WiFi+BLE+GNSS on.",
            "d": {"fw": (2, 5, 0), "bootCount": 1234, "unexpectedResets": 0,
                  "lastResetReason": 5, "gpsTimeouts": 0, "loraJoinFailures": 0,
                  "loraSendFailures": 0, "wifiFailures": 0, "zigbeeFailures": 0,
                  "sensorReadFailures": 0, "gpsConsecFails": 0,
                  "powerFlags": 0, "flags2": 0x01,
                  "lastTtff": 9, "linkMargin": 12, "gwCnt": 3,
                  "otaStatus": 0, "configFlags": 0x0D},
        },
        {
            "name": "diag_struggling",
            "description": "Indoor tracker: GPS never fixes (consec=3), some "
                           "join failures, low battery degraded flag, "
                           "recovery mode latched, no GPS this cycle, no "
                           "fix since boot (TTFF 255), OTA download failed. "
                           "GNSS+BME280 on, Zigbee off.",
            "d": {"fw": (2, 5, 0), "bootCount": 65535, "unexpectedResets": 3,
                  "lastResetReason": 1, "gpsTimeouts": 65535,
                  "loraJoinFailures": 7, "loraSendFailures": 8,
                  "wifiFailures": 9, "zigbeeFailures": 10,
                  "sensorReadFailures": 11, "gpsConsecFails": 3,
                  "powerFlags": 0x01, "flags2": 0x04,
                  "lastTtff": 255, "linkMargin": 255, "gwCnt": 255,
                  "otaStatus": 3, "configFlags": 0x18},
        },
    ]
    for f in diag_frames:
        d = f["d"]
        buf = struct.pack("<BBBBHHBHHHHHHBBBBBBBB",
                          5, d["fw"][0], d["fw"][1], d["fw"][2],
                          d["bootCount"], d["unexpectedResets"],
                          d["lastResetReason"],
                          d["gpsTimeouts"], d["loraJoinFailures"],
                          d["loraSendFailures"], d["wifiFailures"],
                          d["zigbeeFailures"], d["sensorReadFailures"],
                          d["gpsConsecFails"], d["powerFlags"], d["flags2"],
                          d["lastTtff"], d["linkMargin"], d["gwCnt"],
                          d["otaStatus"], d["configFlags"])
        assert len(buf) == 29, len(buf)
        icd["diag_frames"].append({"name": f["name"],
                                   "description": f["description"],
                                   "hex": buf.hex(" ").upper(),
                                   "expected": d})

    def crc16_ccitt(data: bytes) -> int:
        crc = 0xFFFF
        for b in data:
            crc ^= b << 8
            for _ in range(8):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 \
                    else (crc << 1) & 0xFFFF
        return crc

    body = bytes(range(159))  # deterministic stand-in DeviceConfig body
    env = bytes([0x43, 3]) + body + struct.pack("<H", crc16_ccitt(body))
    assert len(env) == 163, len(env)
    icd["config_envelope"].append({
        "name": "config_write_v3_valid",
        "description": "Schema-3 envelope; body is a placeholder sequence "
                       "(real body is the 159-byte DeviceConfig).",
        "hex": env.hex(" ").upper(),
        "crc16": crc16_ccitt(body),
    })
    bad = bytes([0x43, 3]) + body + struct.pack("<H", crc16_ccitt(body) ^ 0xFFFF)
    icd["config_envelope"].append({
        "name": "config_write_v3_bad_crc",
        "description": "Same envelope with corrupted CRC — firmware must "
                       "reject (BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN).",
        "hex": bad.hex(" ").upper(),
    })

    for op, name in [(1, "locate_now"), (2, "send_diag"),
                     (3, "reset_diag"), (4, "ble_window"),
                     (5, "ota_update")]:
        icd["downlink_commands"].append({
            "name": name, "opcode": op,
            "hex": f"4D {op:02X} 00",
        })
    # CFG_SET (ICD 3.5): arg bit7 = value, bits0-3 = flag id.
    for flag_id, flag_name in [(1, "wifi"), (2, "zigbee"), (3, "ble"),
                               (4, "gnss"), (5, "bme280"), (6, "opt3001"),
                               (7, "lis3dh")]:
        icd["downlink_commands"].append({
            "name": f"cfg_set_{flag_name}_on", "opcode": 6,
            "arg": (1 << 7) | flag_id,
            "hex": f"4D 06 {(1 << 7) | flag_id:02X}",
        })
        icd["downlink_commands"].append({
            "name": f"cfg_set_{flag_name}_off", "opcode": 6,
            "arg": flag_id,
            "hex": f"4D 06 {flag_id:02X}",
        })
    # SET_RANGE (ICD 3.6): arg = the kSleepTableMs index (0..8).
    for idx, minutes in enumerate([1, 2, 5, 10, 15, 20, 30, 60, 120]):
        icd["downlink_commands"].append({
            "name": f"set_range_{minutes}min", "opcode": 7,
            "arg": idx,
            "hex": f"4D 07 {idx:02X}",
        })

    (OUT / "icd_v3.json").write_text(json.dumps(icd, indent=2) + "\n")

    h = ["// GENERATED by tools/gen_vectors.py — do not edit by hand.",
         "#pragma once", "#include <cstdint>", "#include <cstddef>", "",
         "namespace minilora { namespace vectors {", "",
         "struct GoldenVector {",
         "    const char* name;",
         "    const uint8_t frame[28];",
         "    int32_t temp; uint32_t hum; uint32_t pres;",
         "    uint16_t bat_mv; uint32_t lux;",
         "    int32_t acc[3];          // decoded raw counts",
         "    int32_t lat; int32_t lon; // microdegrees",
         "    int32_t alt; uint32_t sat; uint32_t time;",
         "    uint8_t awake, sleep, gpio;",
         "};", ""]
    for v in vectors:
        t = v["expected"]
        hx = v["hex"].split()
        frame = ", ".join(f"0x{b}" for b in hx)
        acc = ", ".join(str(a) for a in t["acc"])
        h.append(f"static const GoldenVector {v['name']} {{")
        h.append(f'    "{v["name"]}",')
        h.append(f"    {{{frame}}},")
        h.append(f"    {t['temp']}, {t['hum']}, {t['pres']}, {t['bat_mv']}, {t['lux']},")
        h.append(f"    {{{acc}}},")
        h.append(f"    {t['lat']}, {t['lon']}, {t['alt']}, {t['sat']}, {t['time']},")
        h.append(f"    {t['awake']}, {t['sleep']}, {t['gpio']},")
        h.append("};")
        h.append("")
    h += ["} } // namespace", ""]
    (OUT / "vectors.hpp").write_text("\n".join(h))

    for v in vectors:
        print(f"{v['name']:28s} {v['hex']}")
    print(f"\nwrote {OUT/'payload_v1.json'}, {OUT/'icd_v3.json'} and "
          f"{OUT/'vectors.hpp'}")


if __name__ == "__main__":
    main()
