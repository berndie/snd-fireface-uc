#!/usr/bin/env python3
"""Play a WAV file on chosen Fireface UC output channels.

usage: play_wav.py [-d DEVICE] [-g GAIN_DB] file.wav CH [CH ...]

Channels are numbered from 1 (AN 1 = 1, AN 2 = 2, ...).  File channel i
goes to the i-th listed output; a mono file is copied to every listed
output.  Reads 8/16/24/32-bit integer and 32/64-bit float WAV files.
"""
import argparse
import struct
import sys

import numpy as np
import sounddevice as sd


def read_wav(path):
    """Return (float32 array of shape (frames, channels), sample rate)."""
    with open(path, "rb") as f:
        riff, _, wave = struct.unpack("<4sI4s", f.read(12))
        if riff != b"RIFF" or wave != b"WAVE":
            sys.exit(f"{path}: not a WAV file")
        fmt = None
        while True:
            head = f.read(8)
            if len(head) < 8:
                sys.exit(f"{path}: no data chunk")
            cid, size = struct.unpack("<4sI", head)
            if cid == b"fmt ":
                body = f.read(size + (size & 1))
                tag, nch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
                if tag == 0xfffe:                  # WAVE_FORMAT_EXTENSIBLE
                    tag = struct.unpack("<H", body[24:26])[0]
                fmt = tag, nch, rate, bits
            elif cid == b"data":
                if fmt is None:
                    sys.exit(f"{path}: data before fmt chunk")
                raw = f.read(size)
                break
            else:
                f.seek(size + (size & 1), 1)

    tag, nch, rate, bits = fmt
    width = bits // 8
    raw = raw[:len(raw) // (width * nch) * width * nch]
    if tag == 3 and bits in (32, 64):
        data = np.frombuffer(raw, f"<f{width}").astype(np.float32)
    elif tag == 1 and bits == 8:
        data = (np.frombuffer(raw, np.uint8).astype(np.float32) - 128) / 128
    elif tag == 1 and bits in (16, 32):
        data = np.frombuffer(raw, f"<i{width}").astype(np.float32) / 2 ** (bits - 1)
    elif tag == 1 and bits == 24:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        data = ((b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16) << 8 >> 8) / 2 ** 23
        data = data.astype(np.float32)
    else:
        sys.exit(f"{path}: unsupported format (tag {tag}, {bits} bits)")
    return data.reshape(-1, nch), rate


ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("file")
ap.add_argument("channels", nargs="+", type=int, metavar="CH",
                help="output channel(s), 1-based")
ap.add_argument("-d", "--device", default="Fireface UC",
                help="sounddevice name or index (default: %(default)s)")
ap.add_argument("-g", "--gain", type=float, default=0.0,
                help="gain in dB (default: 0)")
args = ap.parse_args()

data, rate = read_wav(args.file)
if data.shape[1] == 1:
    data = np.repeat(data, len(args.channels), axis=1)
elif data.shape[1] != len(args.channels):
    sys.exit(f"{args.file} has {data.shape[1]} channels but "
             f"{len(args.channels)} outputs were given")
data *= 10 ** (args.gain / 20)

dev = args.device
if not dev.isdigit():
    # prefer the ALSA device over PulseAudio/JACK entries of the same name
    alsa = [i for i, h in enumerate(sd.query_hostapis()) if h["name"] == "ALSA"]
    dev = next((i for i, d in enumerate(sd.query_devices())
                if dev in d["name"] and d["hostapi"] in alsa and
                d["max_output_channels"] > 0), dev)
info = sd.query_devices(int(dev) if str(dev).isdigit() else dev, "output")
bad = [c for c in args.channels if not 1 <= c <= info["max_output_channels"]]
if bad:
    sys.exit(f"channel(s) {bad} out of range 1..{info['max_output_channels']}"
             f" on {info['name']}")

print(f"{args.file}: {len(data) / rate:.1f} s at {rate} Hz -> "
      f"{info['name']} outputs {args.channels}")
try:
    sd.play(data, rate, mapping=args.channels, device=info["index"],
            blocking=True)
except KeyboardInterrupt:
    sd.stop()
