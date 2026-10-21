#!/usr/bin/env python3
"""Verify OUT format / channel order via the hardware loopback (PC mode, 1x/2x)."""
import sys, time, threading
import numpy as np
import usb.core, usb.util
from ffuc_explore import w, r, set_rate, init_hw, VID, PID

def route(dev, nch, k, on):
    # playback k -> output k at unity, output k master at unity
    w(dev, 0x12, 0x2000 if on else 0, 36 * k + 18 + k)
    w(dev, 0x12, 0x2000, 992 + k)
    w(dev, 0x15, 1 if on else 0, k)

def main():
    k = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    rate = int(sys.argv[2]) if len(sys.argv) > 2 else 48000
    alt, nch, mps = (1, 18, 576) if rate <= 48000 else (2, 14, 896)
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    usb.util.claim_interface(dev, 0)
    dev.set_interface_altsetting(0, 1)
    init_hw(dev)
    dev.set_interface_altsetting(0, alt)
    set_rate(dev, rate)
    route(dev, nch, k, True)
    w(dev, 0x14, 0xffff, 0xc000)
    fpu = mps * 8 // (nch * 4)
    amp, f0 = 0.03162, 997.0
    stop = False
    def writer():
        n = 0
        while not stop:
            t = (np.arange(fpu) + n) / rate
            buf = np.zeros((fpu, nch), np.float32)
            buf[:, k] = amp * np.sin(2 * np.pi * f0 * t)
            try:
                dev.write(0x01, buf.tobytes(), timeout=200); n += fpu
            except usb.core.USBError as e:
                print("OUT", e)
    th = threading.Thread(target=writer); th.start()
    chunks = []
    t0 = time.time()
    while time.time() - t0 < 1.0:
        chunks.append(bytes(dev.read(0x82, mps * 8, timeout=200)))
    stop = True; th.join()
    w(dev, 0x13, 0xffff, 0xc000)
    route(dev, nch, k, False)
    data = np.frombuffer(b"".join(chunks), np.float32).reshape(-1, nch)[rate // 4:]
    rms = np.sqrt((data.astype(np.float64) ** 2).mean(axis=0))
    print(f"rate {rate} out ch {k}: capture RMS per channel (dBFS):")
    print("  " + " ".join(f"{i}:{20*np.log10(x+1e-12):6.1f}" for i, x in enumerate(rms)))
    spec = np.abs(np.fft.rfft(data[:, k] * np.hanning(len(data))))
    print(f"  peak freq on ch {k}: {np.argmax(spec) * rate / len(data):.1f} Hz, expected {f0}; sent RMS {20*np.log10(amp/np.sqrt(2)):.1f} dBFS")
    usb.util.release_interface(dev, 0)

main()
