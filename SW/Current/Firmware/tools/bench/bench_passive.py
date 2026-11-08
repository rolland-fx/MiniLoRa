#!/usr/bin/env python3
"""Passive single-open capture — proves/disproves 'device resets itself'.

Context: reset reason 11 is ESP_RST_USB (host-side DTR/RTS artifact), NOT
PWRGLITCH (=14). The earlier ~2-min 'reboot loop' matched the capture
script's 180 s window rotations (port close -> DTR drop -> USB-JTAG reset).
This script opens the port ONCE, holds it, and never rotates windows.

Phase A: wait for port, open once (DTR low), hold 8 min.
         Healthy device: 'GPS timeout after 150 iterations' + 'sleeping'.
Phase B: wait for the 15-min sleep to end, open once more.
         If the chip was woken by the timer (not reset by our open),
         the banner shows reset reason 8 (ESP_RST_DEEPSLEEP) + wake=3.
         If our open resets it, we see 11 again -> tooling still at fault.
"""
import sys, time, glob, json
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from bench_indoor import score
import serial

STAMP = lambda: time.strftime('%H:%M:%S')

def wait_and_open(timeout_s):
    end = time.time() + timeout_s
    while time.time() < end:
        ports = glob.glob('/dev/ttyACM*')
        if ports:
            s = serial.Serial()
            s.port = ports[0]
            s.dtr = False; s.rts = False
            s.open()
            s.dtr = False; s.rts = False   # re-assert AFTER open, just in case
            s.timeout = 0.1
            return s
        time.sleep(0.3)
    return None

def capture(ser, seconds):
    """Hold the port, timestamp gaps > 1 s. Never close mid-capture.
    A read exception (device reset/disconnect) is recorded, not fatal."""
    buf, last_data = b'', time.time()
    end = time.time() + seconds
    while time.time() < end:
        try:
            d = ser.read(4096)
        except serial.SerialException as e:
            buf += f"\n<<READ ERROR {STAMP()}: {e}>>\n".encode()
            return buf, not glob.glob('/dev/ttyACM*')
        if d:
            if time.time() - last_data > 1.0:
                buf += f"\n<<gap {STAMP()}>>\n".encode()
            buf += d
            last_data = time.time()
        elif not glob.glob('/dev/ttyACM*'):
            buf += f"\n<<PORT GONE {STAMP()}>>\n".encode()
            return buf, True          # device left (real sleep or unplug)
    return buf, False                 # window elapsed, port still there

print(f"[{STAMP()}] waiting for device (plug in / press reset)...", flush=True)
ser = wait_and_open(600)
if ser is None:
    print(f"[{STAMP()}] no port within 10 min — device unpowered?", flush=True)
    sys.exit(1)
print(f"[{STAMP()}] opened once — HOLDING, no window rotation", flush=True)

a, a_left = capture(ser, 480)
ser.close()
print(f"[{STAMP()}] phase A done (port_left={a_left})", flush=True)

print(f"[{STAMP()}] phase B: waiting up to 20 min for the wake...", flush=True)
ser = wait_and_open(1200)
if ser is None:
    print(f"[{STAMP()}] no wake within 20 min", flush=True)
    b = b''
else:
    print(f"[{STAMP()}] reopened after sleep", flush=True)
    b, _ = capture(ser, 180)
    ser.close()
print(f"[{STAMP()}] phase B done", flush=True)

log = a.decode('utf-8', 'replace') + "\n===== PHASE B (post-sleep) =====\n" + b.decode('utf-8', 'replace')
open('/tmp/opencode/bench_passive.txt', 'w').write(log)

text = log
checks = {
    "gps_timeout_seen":    "GPS timeout after" in text,
    "sleep_line_seen":     "sleeping" in text,
    "no_panic":            "Guru Meditation" not in text,
    "deep_sleep_wake_8":   "reset reason: 8" in text,
    "usb_reset_11":        "reset reason: 11" in text,
    "warm_wake_3":         "wake=3" in text,
    "boot_count":          text.count('cycle start'),
}
print(json.dumps(checks, indent=2))
boot_n = text.count('cycle start')
verdict = ("DEVICE HEALTHY — prior 'reboot loop' was tooling-induced USB resets"
           if checks['sleep_line_seen'] and boot_n <= 2 else
           "still resetting — inspect annotated log")
print('VERDICT:', verdict)
