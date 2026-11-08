#!/usr/bin/env python3
"""DTR-neutral long capture.

Lesson from the 08:40 reset cascade: pyserial open/close cycles toggle
DTR and each toggle resets the ESP32-C6 USB-Serial-JTAG (~4 resets/s
when polling). Fix: clear HUPCL on the tty (kernel stops asserting DTR
on open / dropping it on close), open ONCE, keep the handle across the
device's sleep cycles, and back off 5 s after any exception.

Captures ~40 min: cold/warm cycle banners, GPS decision line, reset
reasons, wake causes — without resetting the device.
"""
import subprocess, sys, time, glob, json
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from bench_indoor import score
import serial

STAMP = lambda: time.strftime('%H:%M:%S')
LOG = '/tmp/opencode/clean_log.txt'

def append(text):
    with open(LOG, 'a') as f:
        f.write(text)

append(f"\n==== clean capture start {STAMP()} ====\n")

def dtr_neutral_open():
    """stty -hupcl first: no DTR assert on open, no drop on close.
    The port can vanish between glob() and open() (device sleeps) —
    any failure just returns None and we retry."""
    ports = glob.glob('/dev/ttyACM*')
    if not ports:
        return None
    try:
        subprocess.run(['stty', '-F', ports[0], '-hupcl'],
                       capture_output=True)
        ser = serial.Serial()
        ser.port = ports[0]
        ser.timeout = 0.1
        ser.open()          # dtr/rts untouched — HUPCL cleared keeps them idle
        return ser
    except Exception:
        return None

end = time.time() + 2400
ser = None
while time.time() < end:
    if ser is None:
        ser = dtr_neutral_open()
        if ser:
            append(f"<<OPENED {STAMP()} (dtr-neutral)>>\n")
        else:
            time.sleep(0.5)
        continue
    try:
        d = ser.read(4096)
        if d:
            append(d.decode('utf-8', 'replace'))
    except serial.SerialException:
        append(f"<<READ ERROR {STAMP()}>>\n")
        try:
            ser.close()
        except Exception:
            pass
        ser = None
        time.sleep(5)   # generous backoff — no hammering

log = open(LOG, errors='replace').read()
boot_n = log.count('cycle start')
reasons = {}
import re
for m in re.findall(r'reset reason: (\d+)', log):
    reasons[m] = reasons.get(m, 0) + 1
print("reset reasons:", json.dumps(reasons))
print("boots:", boot_n, "| sleeping lines:", log.count('sleeping'),
      "| gps decisions:", log.count('gps decision'),
      "| gps timeouts:", len(re.findall(r'GPS timeout after (\d+)', log)),
      "| gnss nacks:", log.count('GNSS not responding'),
      "| recovery:", log.count('recovery mode'),
      "| panics:", log.count('Guru Meditation'))
print("gps decisions:", re.findall(r'gps decision: .*', log)[:6])
print("gps timeouts:", re.findall(r'GPS timeout after \d+ iterations.*', log)[:4])
print("sleep lines:", re.findall(r'sleeping \d+ ms.*', log)[:4])
print("---- last 2500 chars ----")
print(log[-2500:])
