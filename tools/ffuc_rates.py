#!/usr/bin/env python3
"""Measure IN packet sizes / throughput per sample rate (PC mode)."""
import sys, time, threading
import usb.core, usb.util
from ffuc_explore import w, r, set_rate, init_hw, VID, PID

ALT = {32000: 1, 44100: 1, 48000: 1, 64000: 2, 88200: 2, 96000: 2, 128000: 3, 176400: 3, 192000: 3}

def run(dev, rate, alt, secs=1.0):
    dev.set_interface_altsetting(0, alt)
    mps = dev.get_active_configuration()[(0, alt)][0].wMaxPacketSize
    set_rate(dev, rate)
    w(dev, 0x14, 0xffff, 0xc000)
    stop = False
    outbytes = [0]
    def writer():
        while not stop:
            try:
                outbytes[0] += dev.write(0x01, bytes(mps * 8), timeout=100)
            except usb.core.USBError:
                pass
    t = threading.Thread(target=writer); t.start()
    hist = {}; total = 0; first = None
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            d = bytes(dev.read(0x82, mps * 8, timeout=200))
            hist[len(d)] = hist.get(len(d), 0) + 1; total += len(d)
            if first is None and len(d): first = d
        except usb.core.USBError as e:
            hist[str(e)] = hist.get(str(e), 0) + 1
    el = time.time() - t0
    stop = True; t.join()
    w(dev, 0x13, 0xffff, 0xc000)
    nz = []
    if first:
        words = [int.from_bytes(first[i:i+4], 'little') for i in range(0, min(len(first), 256), 4)]
        nz = [i for i, x in enumerate(words) if x]
    st = r(dev, 0x11, 4).hex()
    print(f"rate {rate:6} alt {alt} mps {mps}: IN {total/el/1e6:.3f} MB/s OUT {outbytes[0]/el/1e6:.3f} MB/s "
          f"hist={dict(list(hist.items())[:6])} status={st} nonzero_words(first 64)={nz[:40]}")

def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    usb.util.claim_interface(dev, 0)
    dev.set_interface_altsetting(0, 1)
    init_hw(dev)
    rates = [int(x) for x in sys.argv[1:]] or list(ALT)
    for rate in rates:
        run(dev, rate, ALT[rate])
    dev.set_interface_altsetting(0, 1)
    set_rate(dev, 48000)
    usb.util.release_interface(dev, 0)

if __name__ == "__main__":
    main()
