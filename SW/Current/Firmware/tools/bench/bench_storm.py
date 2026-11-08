#!/usr/bin/env python3
"""Reset-storm logger: incrementally append EVERYTHING to disk.

The device re-enumerates every 1-60 s. This script polls fast (50 ms),
opens DTR-safe the instant the port appears, and appends timestamped
data to the log after EVERY read - so resets/exceptions/crashes of the
script itself never lose the captured boot banners (reset reasons!).
"""
import sys, time, glob, json
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from bench_indoor import score
import serial

STAMP = lambda: time.strftime('%H:%M:%S')
LOG = '/tmp/opencode/storm_log.txt'

def append(text):
    with open(LOG, 'a') as f:
        f.write(text)

append(f"\n==== storm logger start {STAMP()} ====\n")

end = time.time() + 2700
while time.time() < end:
    ports = glob.glob('/dev/ttyACM*')
    if not ports:
        time.sleep(0.05)
        continue
    try:
        ser = serial.Serial()
        ser.port = ports[0]
        ser.dtr = False; ser.rts = False
        ser.open()
        ser.timeout = 0.05
        append(f"<<OPENED {ports[0]} {STAMP()}>>\n")
        # hold until it dies or 60 s elapse
        t_end = time.time() + 60
        while time.time() < t_end:
            d = ser.read(4096)
            if d:
                append(d.decode('utf-8', 'replace'))
            if not glob.glob('/dev/ttyACM*'):
                append(f"<<PORT GONE {STAMP()}>>\n")
                break
        try:
            ser.close()
        except Exception:
            pass
        append(f"<<CLOSED {STAMP()}>>\n")
    except serial.SerialException as e:
        append(f"<<OPEN FAILED {STAMP()}: {e}>>\n")
        time.sleep(0.2)

log = open(LOG).read()
boot_n = log.count('cycle start')
reasons = {}
import re
for m in re.findall(r'reset reason: (\d+)', log):
    reasons[m] = reasons.get(m, 0) + 1
print("reset reasons seen:", json.dumps(reasons))
print("boots captured:", boot_n)
print("panics:", log.count('Guru Meditation'), "| WDT lines:", log.count('watchdog'))
print("recovery lines:", log.count('recovery mode'))
print("sleep lines:", log.count('sleeping'))
print("---- last 3000 chars ----")
print(log[-3000:])
