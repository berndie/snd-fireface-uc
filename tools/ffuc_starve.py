#!/usr/bin/env python3
"""Does a short OUT underrun latch the outputs into silence?

Streams a looped-back tone on output 0 (48 kHz) with libusb, stalls the OUT
writer once for STALL_MS at 1 s, and reports when the tone is present in the
capture before and after the stall.  usage: ffuc_starve.py STALL_MS
"""
import sys, time, threading
import numpy as np
import usb.core, usb.util
from ffuc_explore import w, r, set_rate, init_hw, VID, PID

stall_ms = float(sys.argv[1]) if len(sys.argv) > 1 else 20
action = sys.argv[2] if len(sys.argv) > 2 else "none"   # recovery tried at 1.5 s
rate, nch, mps = 48000, 18, 576
fpu = mps * 8 // (nch * 4)               # 32 frames per write
dev = usb.core.find(idVendor=VID, idProduct=PID)
usb.util.claim_interface(dev, 0)
dev.set_interface_altsetting(0, 1)
init_hw(dev); set_rate(dev, rate)
w(dev, 0x12, 0x2000, 18); w(dev, 0x12, 0x2000, 992); w(dev, 0x15, 1, 0)
w(dev, 0x14, 0xffff, 0xc000)
stop = False
t0 = time.time()
def writer():
    n = 0; t0 = time.time(); stalled = False
    while not stop:
        if not stalled and time.time() - t0 > 1.0:
            time.sleep(stall_ms / 1000); stalled = True
        t = (np.arange(fpu) + n) / rate
        buf = np.zeros((fpu, nch), np.float32); buf[:, 0] = 0.0316 * np.sin(2 * np.pi * 997 * t)
        try: dev.write(0x01, buf.tobytes(), timeout=200); n += fpu
        except usb.core.USBError as e: print("OUT", e)
ticks = []
def ticker():
    while not stop:
        try: ticks.append((time.time() - t0, bytes(dev.read(0x8b, 4, timeout=100))))
        except usb.core.USBError: pass
        time.sleep(0.005)
if "--tick" in sys.argv:
    usb.util.claim_interface(dev, 5)
    threading.Thread(target=ticker, daemon=True).start()
th = threading.Thread(target=writer); th.start()
chunks = []; st = {}
def act():
    if action == "unmute": w(dev, 0x14, 0xffff, 0xc000)
    elif action == "mute_unmute": w(dev, 0x13, 0xffff, 0xc000); w(dev, 0x14, 0xffff, 0xc000)
    elif action == "rate": set_rate(dev, rate)
    elif action == "dds": 
        p1 = 0x5f5e10000000 // rate; w(dev, 0x1b, p1 >> 16 & 0xffff, (p1 & 0xff00) | 2)
    elif action == "family": w(dev, 0x10, 0x20, 0x30)
    elif action == "init": init_hw(dev)
    elif action == "enable": w(dev, 0x20, 1, 0)
    elif action == "bit11": w(dev, 0x10, 0x0800, 0xb800)
    elif action == "clear_out": dev.clear_halt(0x01)
    elif action == "clear_both": dev.clear_halt(0x01); dev.clear_halt(0x82)
    elif action == "setalt": dev.set_interface_altsetting(0, 1)
    elif action == "chan_init":
        for i in range(18): w(dev, 0x16, 1, i); w(dev, 0x16, 1, i + 0x20)
while time.time() - t0 < 3.0:
    el = time.time() - t0
    for name, at in (("s0.5", 0.5), ("s1.2", 1.2), ("act", 1.5), ("s1.7", 1.7)):
        if el > at and name not in st:
            st[name] = act() if name == "act" else r(dev, 0x11, 4).hex()
    try: chunks.append(bytes(dev.read(0x82, mps * 8, timeout=200)))
    except usb.core.USBError as e: print("IN", e)
stop = True; th.join()
w(dev, 0x13, 0xffff, 0xc000); w(dev, 0x15, 0, 0)
x = np.frombuffer(b"".join(chunks), np.float32).reshape(-1, nch)[:, 0].astype(np.float64)
blk = 480; n = len(x) // blk
db = 20 * np.log10(np.sqrt((x[:n*blk].reshape(n, blk) ** 2).mean(1)) + 1e-15)
on = db > -40
print(f"stall {stall_ms:5.1f} ms, action {action:11}: tone {on[20:95].mean()*100:3.0f}% before stall, "
      f"{on[110:145].mean()*100:3.0f}% after stall, {on[160:290].mean()*100:3.0f}% after action;"
      f" status {st.get('s0.5')} -> {st.get('s1.2')} -> {st.get('s1.7')}")
if ticks:
    flag = [(t, bool(v[3] & 0x80)) for t, v in ticks]
    edges = [(round(t, 3), f) for (t, f), (_, pf) in zip(flag[1:], flag[:-1]) if f != pf]
    print(f"  ticker bit7 transitions (s, set?): {edges[:10]}")
    if "--brief" in sys.argv:
        ticks = []
    import collections
    win = collections.defaultdict(list)
    for t, v in ticks:
        win[int(t * 20)].append(v)
    prev_cnt = None
    for k in sorted(win) if ticks else []:
        vs = win[k]
        cnt = [int.from_bytes(v[:2], "little") for v in vs]
        b2 = sorted({v[2] for v in vs}); b3 = sorted({v[3] for v in vs})
        span = (cnt[-1] - cnt[0]) & 0xffff
        print(f"  {k*50:5d} ms: {len(vs):3d} reads, counter +{span:5d}, byte2 {[hex(x) for x in b2]}, byte3 {[hex(x) for x in b3]}")
usb.util.release_interface(dev, 0)
