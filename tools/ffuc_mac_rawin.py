#!/usr/bin/env python3
"""Record raw Mac-mode IN packets (with OUT running) and report any packet
whose layout does not match "4-byte header + frames stored channel by
channel" (detected by non-zero data in the unconnected digital channels).

usage: ffuc_mac_rawin.py [seconds]
"""
import struct, sys, time
import numpy as np
import usb1
from ffuc_mac import VID, PID, w, set_rate, init_hw

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
nch, in_mps, NPKT, NX = 18, 508, 8, 4
ctx = usb1.USBContext()
h = ctx.openByVendorIDAndProductID(VID, PID)
h.claimInterface(0)
h.setInterfaceAltSetting(0, 0); init_hw(h); h.setInterfaceAltSetting(0, 1); set_rate(h, 48000)
w(h, 0x14, 0xffff, 0xc000)
pk = []; state = {"run": True, "cnt": 0}
def in_cb(t):
    for s, d in t.iterISO():
        pk.append((time.time(), s, bytes(d)))
    if state["run"]:
        t.setIsochronous(0x82, in_mps * NPKT, callback=in_cb, iso_transfer_length_list=[in_mps] * NPKT); t.submit()
def out_cb(t):
    if state["run"]:
        fill(t); t.submit()
def fill(t):
    buf = bytearray(6 * 19 * 4 * NPKT)
    for p in range(NPKT):
        struct.pack_into("<I", buf, p * 6 * 76, state["cnt"]); state["cnt"] = (state["cnt"] + 6) % 0x2000
    t.setIsochronous(0x01, bytes(buf), callback=out_cb, iso_transfer_length_list=[6 * 76] * NPKT)
xs = []
for _ in range(NX):
    t = h.getTransfer(iso_packets=NPKT); t.setIsochronous(0x82, in_mps * NPKT, callback=in_cb, iso_transfer_length_list=[in_mps] * NPKT); t.submit(); xs.append(t)
    t = h.getTransfer(iso_packets=NPKT); fill(t); t.submit(); xs.append(t)
t0 = time.time()
while time.time() - t0 < secs:
    ctx.handleEventsTimeout(0.01)
state["run"] = False
for t in xs:
    try: t.cancel()
    except usb1.USBError: pass
t1 = time.time()
while any(t.isSubmitted() for t in xs) and time.time() - t1 < 1: ctx.handleEventsTimeout(0.05)
w(h, 0x13, 0xffff, 0xc000); h.setInterfaceAltSetting(0, 0); h.releaseInterface(0)

from collections import Counter
print(f"{len(pk)} packets; sizes {Counter(len(d) for _, s, d in pk).most_common(6)}; statuses {Counter(s for _, s, _ in pk)}")
bad = 0
for i, (t, s, d) in enumerate(pk):
    if len(d) < 4 + 72: continue
    n = (len(d) - 4) // 72
    smp = np.frombuffer(d[4:4 + n * 72], np.float32)
    digital = smp[8 * n:]               # channel-major: channels 8..17
    if np.any(digital != 0):
        bad += 1
        if bad <= 4:
            w32 = struct.unpack_from(f"<{len(d)//4}I", d)
            nzw = [j for j, v in enumerate(w32[1:]) if v]
            print(f"packet {i} t={t - t0:.3f}s len {len(d)} (n={n}) hdr {w32[0]:#010x}; prev hdr {struct.unpack_from('<I', pk[i-1][2])[0] if len(pk[i-1][2])>=4 else None!r}")
            print(f"   non-zero word indices (after header): {nzw[:12]} ... {nzw[-6:]}  count {len(nzw)}")
            print(f"   neighbour sizes: {[len(x[2]) for x in pk[i-3:i+4]]}")
print(f"{bad} packets with data in the digital channels")
