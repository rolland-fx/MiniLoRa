#!/usr/bin/env python3
"""Catch 2+ warm wake cycles: poll the port at 0.2 s, capture each awake
window, stop after 2 complete cycles or 30 min. DTR-safe opens only."""
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

all_logs = []
cycles_seen = 0
end = time.time() + 30 * 60
last_activity = None
while time.time() < end and cycles_seen < 3:
    ports = glob.glob('/dev/ttyACM*')
    if not ports:
        time.sleep(0.2); continue
    try:
        ser = open_safe()
        window_end = time.time() + 180
        buf = b''
        connected = True
        while time.time() < window_end and glob.glob('/dev/ttyACM*'):
            d = ser.read(4096)
            if d:
                buf += d
                last_activity = time.time()
        ser.close()
        if buf:
            text = buf.decode('utf-8', 'replace')
            all_logs.append(text)
            n = text.count('cycle start')
            cycles_seen += n
            print(f"[{time.strftime('%H:%M:%S')}] captured window: {n} cycle(s), "
                  f"{len(buf)} bytes", flush=True)
    except serial.SerialException:
        pass
    time.sleep(0.2)

log = '\n'.join(all_logs)
print('==== combined score ====')
res = score(log)
print(json.dumps(res, indent=2))
failed = [k for k, v in res.items() if not v['ok']]
print('RESULT:', 'PASS' if not failed else f'FAIL: {failed}')
print('==== key lines ====')
for line in log.splitlines():
    if any(k in line for k in ('boot (reset', 'cycle start', 'GPS ', 'sleeping',
                               'OTA', 'asset', 'motion', 'skipped')):
        print(line)
open('/tmp/opencode/bench_log5.txt', 'w').write(log)
