#!/usr/bin/env python3
"""Round-trip latency through the hardware loopback (output 1 -> input 1).

Plays a click through a duplex sounddevice stream and measures where it
comes back in the capture.  usage: latency.py [rate] [blocksize] [requested latency in ms]
"""
import sys

import numpy as np
import sounddevice as sd
import usb.core

rate = int(sys.argv[1]) if len(sys.argv) > 1 else 48000
block = int(sys.argv[2]) if len(sys.argv) > 2 else 64
lat = float(sys.argv[3]) / 1000 if len(sys.argv) > 3 else "low"   # ms
dev = next(i for i, d in enumerate(sd.query_devices())
           if "Fireface UC" in d["name"] and d["hostapi"] == 0)
usbdev = (usb.core.find(idVendor=0x0424, idProduct=0x3fc5) or usb.core.find(idVendor=0x2a39, idProduct=0x3fc6))

n = rate // 2
out = np.zeros((n, 2), np.float32)
out[rate // 10, 0] = 0.5
usbdev.ctrl_transfer(0x40, 0x15, 1, 0, None)
try:
    rec = sd.playrec(out, samplerate=rate, channels=2, dtype="float32",
                     device=(dev, dev), blocksize=block, latency=lat, blocking=True)
finally:
    usbdev.ctrl_transfer(0x40, 0x15, 0, 0, None)
lag = int(np.argmax(np.abs(rec[:, 0]))) - rate // 10
print(f"{rate} Hz, blocksize {block:3}, latency {sys.argv[3] if len(sys.argv) > 3 else 'low'} ms: "
      f"round trip {lag} frames = {lag / rate * 1e3:.2f} ms")
