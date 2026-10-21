#!/usr/bin/env python3
"""Probe 4x-rate streaming (alt 3) using all four interrupt endpoints."""
import sys, time, threading
import usb.core, usb.util
from ffuc_explore import w, r, set_rate, init_hw, VID, PID

def main():
    rate = int(sys.argv[1]) if len(sys.argv) > 1 else 192000
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    usb.util.claim_interface(dev, 0)
    dev.set_interface_altsetting(0, 1)
    init_hw(dev)
    dev.set_interface_altsetting(0, 3)
    set_rate(dev, rate)
    w(dev, 0x14, 0xffff, 0xc000)
    stop = False
    stats = {}
    first = {}
    def out(ep):
        n = 0
        while not stop:
            try: n += dev.write(ep, bytes(768 * 8), timeout=100)
            except usb.core.USBError as e: stats[f"out{ep:#x} {e}"] = stats.get(f"out{ep:#x} {e}", 0) + 1
        stats[f"out{ep:#x} bytes"] = n
    def inp(ep):
        n = 0
        while not stop:
            try:
                d = bytes(dev.read(ep, 768 * 8, timeout=100)); n += len(d)
                first.setdefault(ep, d)
            except usb.core.USBError as e: stats[f"in{ep:#x} {e}"] = stats.get(f"in{ep:#x} {e}", 0) + 1
        stats[f"in{ep:#x} bytes"] = n
    ts = [threading.Thread(target=f, args=(e,)) for f, e in ((out, 0x01), (out, 0x03), (inp, 0x82), (inp, 0x84))]
    for t in ts: t.start()
    time.sleep(1.0); stop = True
    for t in ts: t.join()
    w(dev, 0x13, 0xffff, 0xc000)
    print(rate, stats, "status", r(dev, 0x11, 4).hex())
    for ep, d in first.items():
        words = [int.from_bytes(d[i:i+4], 'little') for i in range(0, min(len(d), 192), 4)]
        print(f"ep {ep:#x} len {len(d)} nonzero idx {[i for i, x in enumerate(words) if x]}")
    dev.set_interface_altsetting(0, 1); set_rate(dev, 48000)
    usb.util.release_interface(dev, 0)

main()
