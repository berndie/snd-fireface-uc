#!/usr/bin/env python3
"""Exploration of the Fireface UC PC-mode streaming endpoints."""
import sys, time, threading
import usb.core, usb.util

VID, PID = 0x0424, 0x3fc5

def w(dev, req, val, idx):
    dev.ctrl_transfer(0x40, req, val, idx, None, timeout=1000)

def r(dev, req, n=4, idx=0):
    return bytes(dev.ctrl_transfer(0xc0, req, 0, idx, n, timeout=1000))

def set_rate(dev, rate):
    flag, p2 = {32000: (0, 0), 64000: (0, 0), 128000: (0, 0),
                44100: (1, 0x10), 88200: (1, 0x10), 176400: (1, 0x10)}.get(rate, (2, 0x20))
    p1 = 0x5f5e10000000 // rate
    if rate > 114000: p1 <<= 2
    elif rate > 57000: p1 <<= 1
    p1 &= 0xffffffff
    w(dev, 0x1b, p1 >> 16, (p1 & 0xff00) | flag)
    w(dev, 0x10, p2, 0x30)

def init_hw(dev):
    for i in range(18):
        w(dev, 0x16, 1, i)
        w(dev, 0x16, 1, i + 0x20)
    w(dev, 0x10, 0x0800, 0xb800)
    w(dev, 0x20, 1, 0)

def main():
    alt = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    rate = int(sys.argv[2]) if len(sys.argv) > 2 else 48000
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    for i in range(6):
        if dev.is_kernel_driver_active(i):
            dev.detach_kernel_driver(i)
    usb.util.claim_interface(dev, 0)
    dev.set_interface_altsetting(0, alt)
    print("rev", r(dev, 0x1c).hex(), "status", r(dev, 0x11, 16).hex(' '))
    init_hw(dev)
    set_rate(dev, rate)
    print("after rate: status", r(dev, 0x11, 16).hex(' '), "0x10:", r(dev, 0x10).hex())
    w(dev, 0x14, 0xffff, 0xc000)   # unmute
    stop = False
    def writer():
        pkt = bytes(dev.get_active_configuration()[(0, alt)][0].wMaxPacketSize)
        n = 0
        while not stop:
            try:
                dev.write(0x01, pkt * 8, timeout=100); n += 1
            except usb.core.USBError as e:
                print("OUT err", e); time.sleep(0.05)
        print("OUT writes:", n)
    t = threading.Thread(target=writer); t.start()
    sizes = {}
    first = []
    t0 = time.time()
    try:
        while time.time() - t0 < 2.0:
            try:
                d = bytes(dev.read(0x82, 576*8, timeout=200))
                sizes[len(d)] = sizes.get(len(d), 0) + 1
                if len(first) < 4: first.append(d)
            except usb.core.USBError as e:
                sizes[str(e)] = sizes.get(str(e), 0) + 1
    finally:
        stop = True; t.join()
        w(dev, 0x13, 0xffff, 0xc000)
    print("IN sizes histogram:", sizes)
    for d in first:
        print(len(d), d[:160].hex(' '))
    print("status end", r(dev, 0x11, 16).hex(' '))
    usb.util.release_interface(dev, 0)

if __name__ == "__main__":
    main()
