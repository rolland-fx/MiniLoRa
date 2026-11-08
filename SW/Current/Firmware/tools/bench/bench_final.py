#!/usr/bin/env python3
"""Full post-flash verification: cold-cycle tail + warm wake cycle.

Phase 1 (5 min): capture the tail of the cold cycle (150 s GPS budget ->
timeout line -> sleep line -> port vanishes).
Phase 2 (25 min): watch for the port to reappear (15 min sleep) and
capture the warm cycle (15 s GPS budget, backoff counters, wake=3).
Scores both against the indoor checklist; saves logs.
"""
import sys, time, glob, json
sys.path.insert(0, '/tmp/opencode')
from bench_indoor import score
import serial

def open_safe():
    ser = serial.Serial()
    ser.port = glob.glob('/dev/ttyACM*')[0]
    ser.dtr = False; ser.rts = False
    ser.open()
    ser.dtr = False; ser.rts = False
    ser.timeout = 0.1
    return ser

def capture_until(end):
    buf = b''
    while time.time() < end:
        ports = glob.glob('/dev/ttyACM*')
        if not ports:
            time.sleep(0.2); continue
        try:
            ser = open_safe()
            while time.time() < end and glob.glob('/dev/ttyACM*'):
                d = ser.read(4096)
                if d: buf += d
            ser.close()
            return buf, True   # port vanished = device slept
        except serial.SerialException:
            time.sleep(0.2)
    return buf, False

# ---- phase 1: cold-cycle tail (device just booted from the flash) ----
print(f"[{time.strftime('%H:%M:%S')}] phase 1: cold-cycle tail (5 min)", flush=True)
cold, cold_slept = capture_until(time.time() + 300)
cold_text = cold.decode('utf-8', 'replace')
print(cold_text[-1800:], flush=True)
print(f"[{time.strftime('%H:%M:%S')}] phase 1 done (slept={cold_slept})", flush=True)

# ---- phase 2: warm wake cycle ----
print(f"[{time.strftime('%H:%M:%S')}] phase 2: waiting for warm wake (25 min)", flush=True)
warm, warm_slept = capture_until(time.time() + 25 * 60)
warm_text = warm.decode('utf-8', 'replace')
print(warm_text[-1800:], flush=True)
print(f"[{time.strftime('%H:%M:%S')}] phase 2 done", flush=True)

log = cold_text + "\n===== WARM =====\n" + warm_text
open('/tmp/opencode/bench_final.txt', 'w').write(log)

# ---- scoring ----
print('==== cold-cycle score ====', flush=True)
cold_score = score(cold_text)
print(json.dumps(cold_score, indent=2), flush=True)

# warm-cycle specifics
warm_checks = {
    "warm_boot_seen": "cycle start" in warm_text,
    "warm_wake_timer": "wake=3" in warm_text,
    "warm_gps_15s": "GPS timeout after 15 iterations" in warm_text,
    "sleep_line": "sleeping" in warm_text,
    "ota_confirmed": "OTA image confirmed" in warm_text,
    "led_fix_no_crash": "Guru Meditation" not in warm_text,
}
print('==== warm-cycle checks ====', flush=True)
print(json.dumps(warm_checks, indent=2), flush=True)

cold_fail = [k for k, v in cold_score.items() if not v['ok']]
warm_fail = [k for k, v in warm_checks.items() if not v]
print('COLD:', 'PASS' if not cold_fail else f'FAIL {cold_fail}', flush=True)
print('WARM:', 'PASS' if not warm_fail else f'FAIL {warm_fail}', flush=True)
