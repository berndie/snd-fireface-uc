#!/usr/bin/env python3
"""End-to-end test of the snd-fireface-uc ALSA driver through PortAudio.

Plays a tone on one playback channel with the device's hardware loopback
enabled for the matching output, records all channels at the same time,
and checks that the tone comes back on exactly that capture channel.

The loopback is switched with a vendor control transfer to the device
(recipient device, so no USB interface needs to be claimed while the
kernel driver is bound).  The tone is -30 dBFS on the chosen output.

usage: test_alsa.py [--rates 44100,48000,96000,192000] [--dtype float32]
"""
import argparse
import sys

import numpy as np
import sounddevice as sd
import usb.core

AMP = 0.0316        # -30 dBFS peak
FREQ = 997.0
MAX_CH = {32000: 18, 44100: 18, 48000: 18, 64000: 14, 88200: 14,
          96000: 14, 128000: 12, 176400: 12, 192000: 12}


def find_device():
    for i, d in enumerate(sd.query_devices()):
        if "Fireface UC" in d["name"] and d["hostapi"] == 0:
            return i, d
    sys.exit("no Fireface UC ALSA device found (is snd-fireface-uc loaded?)")


def loopback(usbdev, out, on):
    usbdev.ctrl_transfer(0x40, 0x15, 1 if on else 0, out, None, timeout=1000)


def run(dev_index, usbdev, rate, k, dtype, nch):
    n = int(rate * 0.6)
    t = np.arange(n) / rate
    out = np.zeros((n, nch), np.float32)
    out[:, k] = AMP * np.sin(2 * np.pi * FREQ * t)
    if dtype == "int32":
        out = (out * 2**31).astype(np.int32)
    loopback(usbdev, k, True)
    try:
        rec = sd.playrec(out, samplerate=rate, channels=nch, dtype=dtype,
                         device=(dev_index, dev_index), blocking=True)
    finally:
        loopback(usbdev, k, False)
    rec = rec.astype(np.float64)
    if dtype == "int32":
        rec /= 2**31
    tail = rec[n // 2:]                     # skip the stream latency
    rms = np.sqrt((tail ** 2).mean(axis=0))
    db = 20 * np.log10(rms + 1e-15)
    expected = 20 * np.log10(AMP / np.sqrt(2))
    others = np.delete(db, k)
    ok = abs(db[k] - expected) < 0.5 and others.max() < expected - 40
    print(f"  {rate:6} Hz {dtype:7} {nch:2} ch  out/in ch {k:2}: "
          f"{db[k]:6.1f} dBFS (expect {expected:.1f}), "
          f"loudest other {others.max():6.1f} dBFS  {'OK' if ok else 'FAIL'}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", default="44100,48000,96000,192000",
                    help="comma-separated; any of 32000 44100 48000 64000 88200 96000 128000 176400 192000")
    ap.add_argument("--dtype", default="float32,int32")
    args = ap.parse_args()

    dev_index, info = find_device()
    print(f"PortAudio device {dev_index}: {info['name']} "
          f"(in {info['max_input_channels']}, out {info['max_output_channels']})")
    usbdev = (usb.core.find(idVendor=0x0424, idProduct=0x3fc5) or usb.core.find(idVendor=0x2a39, idProduct=0x3fc6))
    if usbdev is None:
        sys.exit("Fireface UC not found on USB")

    results = []
    for rate in [int(r) for r in args.rates.split(",")]:
        nch = MAX_CH[rate]
        for dtype in args.dtype.split(","):
            for k in sorted({0, 1, nch // 2, nch - 1}):
                results.append(run(dev_index, usbdev, rate, k, dtype, nch))
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
