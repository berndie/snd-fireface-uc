#!/usr/bin/env python3
"""Duplex timeline diagnostic: where does a looped-back tone drop out?

Runs a callback-driven duplex stream for a few seconds with a tone on one
channel and the device's hardware loopback on that output, then reports
PortAudio's xrun flags and every 10 ms block in which the tone is missing.

usage: diag_duplex.py [rate] [channel] [seconds] [blocksize] [--all]

--all enables the loopback on every output, to see where a tone went.
"""
import sys
import threading

import numpy as np
import sounddevice as sd
import usb.core

rate = int(sys.argv[1]) if len(sys.argv) > 1 else 48000
k = int(sys.argv[2]) if len(sys.argv) > 2 else 0
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
blocksize = int(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4].isdigit() else 0
nch = {44100: 18, 48000: 18, 96000: 14, 192000: 12}[rate]
AMP, F0 = 0.0316, 997.0

dev = next(i for i, d in enumerate(sd.query_devices())
           if "Fireface UC" in d["name"] and d["hostapi"] == 0)
usbdev = (usb.core.find(idVendor=0x0424, idProduct=0x3fc5) or usb.core.find(idVendor=0x2a39, idProduct=0x3fc6))

total = int(rate * secs)
rec = np.zeros((total, nch), np.float32)
flags = {"input_overflow": 0, "input_underflow": 0,
         "output_overflow": 0, "output_underflow": 0, "priming_output": 0}
state = {"n": 0}
done = threading.Event()


def cb(indata, outdata, frames, time, status):
    for f in flags:
        if getattr(status, f):
            flags[f] += 1
    n = state["n"]
    t = (np.arange(frames) + n) / rate
    outdata[:] = 0
    outdata[:, k] = AMP * np.sin(2 * np.pi * F0 * t)
    m = min(frames, total - n)
    rec[n:n + m] = indata[:m]
    state["n"] = n + frames
    if state["n"] >= total:
        raise sd.CallbackStop


loop_all = "--all" in sys.argv
for o in (range(nch) if loop_all else [k]):
    usbdev.ctrl_transfer(0x40, 0x15, 1, o, None, timeout=1000)
try:
    with sd.Stream(device=(dev, dev), samplerate=rate, channels=nch,
                   dtype="float32", blocksize=blocksize, callback=cb,
                   finished_callback=done.set) as st:
        lat = st.latency
        done.wait(secs + 5)
finally:
    for o in (range(nch) if loop_all else [k]):
        usbdev.ctrl_transfer(0x40, 0x15, 0, o, None, timeout=1000)

blk = rate // 100
x = rec[:, k].astype(np.float64)
nblk = len(x) // blk
rms = np.sqrt((x[:nblk * blk].reshape(nblk, blk) ** 2).mean(axis=1))
db = 20 * np.log10(rms + 1e-15)
present = db > -40
first = int(np.argmax(present)) if present.any() else -1
missing = [i for i in range(first, nblk) if not present[i]] if first >= 0 else []
print(f"rate {rate} ch {k} latency in/out {lat[0]*1e3:.1f}/{lat[1]*1e3:.1f} ms, flags {flags}")
print(f"tone first seen at block {first} ({first*10} ms); "
      f"{len(missing)} of {nblk - max(first, 0)} later 10 ms blocks missing")
if missing:
    runs, start = [], missing[0]
    for a, b in zip(missing, missing[1:] + [None]):
        if b != a + 1:
            runs.append((start, a))
            start = b
    print("missing ranges (ms):", [(s * 10, (e + 1) * 10) for s, e in runs][:20])
    # Where did the tone go?  Per-channel level over the first missing range.
    s, e = runs[0][0] * blk, (runs[0][1] + 1) * blk
    seg = rec[s:e].astype(np.float64)
    lv = 20 * np.log10(np.sqrt((seg ** 2).mean(axis=0)) + 1e-15)
    ok = rec[first * blk:runs[0][0] * blk].astype(np.float64)
    lv_ok = 20 * np.log10(np.sqrt((ok ** 2).mean(axis=0)) + 1e-15)
    print("per-channel dBFS before dropout:", " ".join(f"{i}:{v:.0f}" for i, v in enumerate(lv_ok)))
    print("per-channel dBFS during dropout:", " ".join(f"{i}:{v:.0f}" for i, v in enumerate(lv)))
