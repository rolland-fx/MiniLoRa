#!/usr/bin/env python3
"""Wait for the device's next wake cycle and capture it (DTR-safe open)."""
import sys, time, glob, json
sys.path.insert(0, '/tmp/opencode')
from bench_indoor import score
import serial

SAFE = dict(dtr=False, rts=False)

def open_safe():
    ser = serial.Serial()
    ser.port = glob.glob('/dev/ttyACM*')[0]
    ser.dtr = False; ser.rts = False
    ser.open()
    ser.dtr = False; ser.rts = False
    return ser

log_parts = []
end = time.time() + 25 * 60  # 25 min: must span one 15-min sleep + wake
while time.time() < end:
    ports = glob.glob('/dev/ttyACM*')
    if not ports:
        time.sleep(1.0); continue
    try:
        ser = open_safe()
        ser.timeout = 0.2
        while time.time() < end and glob.glob('/dev/ttyACM*'):
            d = ser.read(4096)
            if d: log_parts.append(d)
        ser.close()
    except serial.SerialException:
        pass
    time.sleep(1.0)

log = b''.join(log_parts).decode('utf-8', 'replace')
print('==== captured ====')
print(log[-4000:])
print('==== score ====')
res = score(log)
print(json.dumps(res, indent=2))
failed = [k for k, v in res.items() if not v['ok']]
print('RESULT:', 'PASS' if not failed else f'FAIL: {failed}')
open('/tmp/opencode/bench_log3.txt', 'w').write(log)
