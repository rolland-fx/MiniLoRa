#!/usr/bin/env python3
"""Golden-vector check (payload v1 + ICD v3) — mirrors decoder.js math.

Run from SW/Current/TTN:  python3 check_vectors.py
There is also check_vectors.js (same assertions, needs Node). The JS one is
authoritative for the decoder; this one validates the vectors and serves as
the no-Node fallback. Vectors come from Firmware/tools/gen_vectors.py.
"""
import json
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

VEC = Path(__file__).resolve().parent.parent / "Firmware" / "host_tests" / "vectors"
fails = 0


def check(name, cond, detail=""):
    global fails
    if cond:
        print(f"  ok    {name}")
    else:
        fails += 1
        print(f"  FAIL  {name}{(' — ' + str(detail)) if detail else ''}")


def decode_v1(b):
    temp = b[0] - 256 if b[0] > 127 else b[0]
    pres = b[2] | b[3] << 8
    bat = round(2500 + b[4] * 1800 / 255)
    lux = b[5] | b[6] << 8
    lat = struct.unpack("<i", bytes(b[10:14]))[0] / 1e6
    lon = struct.unpack("<i", bytes(b[14:18]))[0] / 1e6
    alt_raw = b[18] | b[19] << 8
    alt = (alt_raw - (0x10000 if alt_raw >= 0x8000 else 0)) - 1000
    sat = b[20]
    t = struct.unpack("<I", bytes(b[21:25]))[0]
    gps_valid = not all(x == 0 for x in b[10:18]) or sat != 0
    return dict(temp=temp, hum=b[1], pres=pres, bat=bat, lux=lux, lat=lat,
                lon=lon, alt=alt, sat=sat, time=t, gpio=b[27],
                gps_valid=gps_valid)


def decode_diag(b):
    assert b[0] == 0x04, f"schema {b[0]}"
    assert len(b) == 28, len(b)
    return dict(fw=".".join(map(str, b[1:4])),
                boot=struct.unpack("<H", bytes(b[4:6]))[0],
                unexp=struct.unpack("<H", bytes(b[6:8]))[0],
                reset=b[8], gps_to=struct.unpack("<H", bytes(b[9:11]))[0],
                consec=b[21], power=b[22], flags2=b[23],
                ttff=b[24], margin=b[25], gw=b[26], ota=b[27])


pv1 = json.loads((VEC / "payload_v1.json").read_text())
print("payload_v1.json")
for v in pv1["vectors"]:
    d = decode_v1([int(x, 16) for x in v["hex"].split()])
    e = v["expected"]
    check(f"{v['name']}: temp", d["temp"] == e["temp"], d["temp"])
    check(f"{v['name']}: hum", d["hum"] == e["hum"])
    check(f"{v['name']}: pres", d["pres"] == e["pres"])
    # Battery quantizes to 255 steps over 1800 mV (~7 mV). Saturating
    # vectors: v2 clamps at the map edges (v1 wraps — that's the divergence).
    bat_exp = e["bat_mv"]
    if v.get("saturating"):
        bat_exp = min(4300, max(2500, bat_exp))
    # one quantization step = 1800/255 ≈ 7.06 mV
    check(f"{v['name']}: bat_mV", abs(d["bat"] - bat_exp) <= 8,
          f"{d['bat']} vs {bat_exp}")
    check(f"{v['name']}: lux", d["lux"] == e["lux"])
    check(f"{v['name']}: lat", d["lat"] == e["lat"] / 1e6, d["lat"])
    check(f"{v['name']}: lon", d["lon"] == e["lon"] / 1e6, d["lon"])
    check(f"{v['name']}: alt", d["alt"] == e["alt"], d["alt"])
    check(f"{v['name']}: sat", d["sat"] == e["sat"])
    t = datetime.fromtimestamp(d["time"], tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    check(f"{v['name']}: time",
          t == datetime.fromtimestamp(e["time"], tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), t)
    check(f"{v['name']}: gpio", d["gpio"] == e["gpio"])

icd = json.loads((VEC / "icd_v3.json").read_text())
print("icd_v3.json — diag frames")
for f in icd["diag_frames"]:
    d = decode_diag([int(x, 16) for x in f["hex"].split()])
    e = f["expected"]
    check(f"{f['name']}: fw", d["fw"] == ".".join(map(str, e["fw"])), d["fw"])
    check(f"{f['name']}: boot", d["boot"] == e["bootCount"])
    check(f"{f['name']}: unexp", d["unexp"] == e["unexpectedResets"])
    check(f"{f['name']}: reset", d["reset"] == e["lastResetReason"])
    check(f"{f['name']}: gps_to", d["gps_to"] == e["gpsTimeouts"])
    check(f"{f['name']}: consec", d["consec"] == e["gpsConsecFails"])
    check(f"{f['name']}: power", d["power"] == e["powerFlags"])
    check(f"{f['name']}: flags2", d["flags2"] == e["flags2"])
    check(f"{f['name']}: ttff", d["ttff"] == e["lastTtff"])
    check(f"{f['name']}: margin", d["margin"] == e["linkMargin"])
    check(f"{f['name']}: gw", d["gw"] == e["gwCnt"])
    check(f"{f['name']}: ota", d["ota"] == e["otaStatus"])

print("icd_v3.json — config envelope")
env = bytes.fromhex(icd["config_envelope"][0]["hex"].replace(" ", ""))


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


check("envelope: magic/schema", env[0] == 0x43 and env[1] == 3)
check("envelope: crc", crc16(env[2:161]) == struct.unpack("<H", env[161:163])[0])

print("icd_v3.json — downlink commands")
# ICD section 3: [0x4D][opcode][arg] on fPort 3 — what the app's Remote tab
# sends and what the TTN downlink decoder must show.
DL_NAMES = {1: "LOCATE_NOW", 2: "SEND_DIAG", 3: "RESET_DIAG",
            4: "BLE_WINDOW", 5: "OTA_UPDATE"}
for c in icd["downlink_commands"]:
    b = [int(x, 16) for x in c["hex"].split(" ")]
    check(f'{c["name"]}: framing', b[0] == 0x4D and len(b) == 3 and b[2] == 0, b)
    check(f'{c["name"]}: opcode matches ICD', b[1] == c["opcode"], b)
    check(f'{c["name"]}: opcode is a known command', b[1] in DL_NAMES, b)

if fails:
    print(f"\n{fails} CHECK(S) FAILED")
    sys.exit(1)
print("\nall vector checks passed")
