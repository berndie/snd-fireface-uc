#!/usr/bin/env python3
"""Round-trip stability through the hardware loopback (output 1 -> input 1).

Plays a click every 250 ms through a duplex sounddevice stream and reports
the round-trip lag of each click.  A constant lag means the playback stream
reaches the outputs without gaps or jumps; any change is a glitch.

usage: click_lag.py [rate] [seconds] [blocksize]
"""
import collections
import sys

import numpy as np
import sounddevice as sd
import usb.core

rate = int(sys.argv[1]) if len(sys.argv) > 1 else 48000
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5
block = int(sys.argv[3]) if len(sys.argv) > 3 else 64
dev = next(i for i, d in enumerate(sd.query_devices())
           if "Fireface UC" in d["name"] and d["hostapi"] == 0)
usbdev = (usb.core.find(idVendor=0x0424, idProduct=0x3fc5) or
          usb.core.find(idVendor=0x2a39, idProduct=0x3fc6))

period = rate // 4
n = int(rate * secs)
out = np.zeros((n, 2), np.float32)
sent = np.arange(period, n - period, period)
out[sent, 0] = 0.5
usbdev.ctrl_transfer(0x40, 0x15, 1, 0, None)      # loopback output 1 -> input 1
try:
    rec = sd.playrec(out, samplerate=rate, channels=2, dtype="float32",
                     device=(dev, dev), blocksize=block, latency="low",
                     blocking=True)
finally:
    usbdev.ctrl_transfer(0x40, 0x15, 0, 0, None)

got = np.flatnonzero(np.abs(rec[:, 0]) > 0.25)
lags = []
for s in sent:
    after = got[(got >= s) & (got < s + period)]
    lags.append(int(after[0] - s) if len(after) else None)
hist = collections.Counter(lags)
print(f"{rate} Hz, {len(sent)} clicks: lags {dict(hist.most_common(5))}, "
      f"{len(got)} click samples captured"
      f"{'  STABLE' if len(hist) == 1 and None not in hist else '  UNSTABLE'}")
if len(hist) > 1:
    print("  sequence:", lags)
