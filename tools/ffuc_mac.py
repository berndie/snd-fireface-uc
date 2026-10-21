#!/usr/bin/env python3
"""Exploration of the Fireface UC Mac-mode (isochronous) streaming.

Streams IN and OUT on interface 0 with libusb isochronous transfers:

  IN  packet: 4-byte header + N frames, samples channel-major (all of
      channel 1, then channel 2, ...), float32 LE
  OUT packet: N frames of (1 header word + channels) float32 LE; the
      header word is 0 except in the first frame, where it holds a running
      frame counter (mod 0x2000)

OUT frame counts copy the IN counts (implicit feedback) unless --fixed.
Plays a tone on CHANNEL with the hardware loopback enabled for that output
and reports the per-channel capture levels.

usage: ffuc_mac.py [rate] [channel] [--fixed] [--no-counter] [--in-only]
                   [--offset=N] [--stall] [--mirror=M]

--offset=N waits for the first IN packet and starts the OUT counter N frames
ahead of the device position reported in the IN header (bits 5-17), and
plays a click to measure where the output lands relative to the capture.

--mirror=M does what the kernel driver does: OUT packet k carries the frame
count of IN packet k - D (D = (NXFER + 1) * NPKT) and is placed at that
packet's position + D * rate / 8000 + M; earlier OUT packets are empty.
"""
import collections
import os
import struct
import sys
import time

import numpy as np
import usb1


VID, PID = 0x2a39, 0x3fc6
MODES = {  # alt, channels, OUT max packet, IN max packet, max frames per packet
    1: (1, 18, 532, 508, 7),
    2: (2, 14, 780, 732, 13),
    3: (3, 12, 1304, 1208, 25),
}
NPKT = int(os.environ.get("NPKT", 8))
NXFER = int(os.environ.get("NXFER", 4))
AMP, F0 = 0.0316, 997.0


def mode_for(rate):
    return MODES[1 if rate <= 48000 else 2 if rate <= 96000 else 3]


def w(h, req, val, idx):
    h.controlWrite(0x40, req, val, idx, b"", timeout=1000)


def r(h, req):
    return int.from_bytes(h.controlRead(0xc0, req, 0, 0, 4, timeout=1000), "little")


def set_rate(h, rate):
    family = 0 if rate in (32000, 64000, 128000) else 1 if rate in (44100, 88200, 176400) else 2
    dds = 0x5f5e10000000 // rate
    if rate > 114000:
        dds <<= 2
    elif rate > 57000:
        dds <<= 1
    dds &= 0xffffffff
    w(h, 0x1b, dds >> 16, (dds & 0xff00) | family)
    w(h, 0x10, family << 4, 0x30)


def init_hw(h):
    for i in range(18):
        w(h, 0x16, 1, i)
        w(h, 0x16, 1, i + 0x20)
    w(h, 0x10, 0x0800, 0xb800)
    w(h, 0x20, 1, 0)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rate = int(args[0]) if args else 48000
    k = int(args[1]) if len(args) > 1 else 0
    fixed = "--fixed" in sys.argv
    counter_on = "--no-counter" not in sys.argv
    in_only = "--in-only" in sys.argv
    stall = "--stall" in sys.argv    # hold all OUT transfers for 20 ms at 1 s
    offset = next((int(a.split("=")[1]) for a in sys.argv if a.startswith("--offset=")), None)
    mirror = next((int(a.split("=")[1]) for a in sys.argv if a.startswith("--mirror=")), None)
    if mirror is not None:
        offset = mirror
    inring = []                               # (position, frames) per IN packet
    alt, nch, out_mps, in_mps, maxf = mode_for(rate)
    nominal = rate / 8000.0

    ctx = usb1.USBContext()
    h = ctx.openByVendorIDAndProductID(VID, PID)
    if h is None:
        sys.exit("Fireface UC (Mac mode) not found")
    if h.kernelDriverActive(0):
        h.detachKernelDriver(0)
    h.claimInterface(0)
    h.setInterfaceAltSetting(0, 0)
    init_hw(h)
    h.setInterfaceAltSetting(0, alt)
    set_rate(h, rate)
    w(h, 0x12, 0x2000, 36 * k + 18 + k)      # playback k -> output k
    w(h, 0x12, 0x2000, 992 + k)
    w(h, 0x15, 1, k)                          # loopback output k -> input k
    w(h, 0x14, 0xffff, 0xc000)                # unmute

    st = dict(running=True, out_pos=0, counter=0 if offset is None or mirror is not None else None,
              in_next=None, debt=0, skip=0, out_filled=0, mirror_miss=0, in_last_end=0, frac=0.0, in_frames=0,
              in_pkts=0, out_pkts=0, in_err=collections.Counter(),
              out_err=collections.Counter(), hdrs=[], sizes=collections.Counter())
    fb = collections.deque()                  # frame counts seen on IN
    cap = []

    def next_counts():
        counts = []
        for _ in range(NPKT):
            if not fixed and fb:
                n = fb.popleft()
            else:
                st["frac"] += nominal
                n = int(st["frac"])
                st["frac"] -= n
            # libusb1 refuses empty iso packets: send 1 frame instead and
            # take it off a later packet, so the total stays right
            n += st["debt"]
            st["debt"] = 0
            if n <= 0:
                st["debt"] = n - 1
                n = 1
            counts.append(min(n, maxf))
        return counts

    D = (NXFER + 1) * NPKT
    lead = round(D * rate / 8000) + (mirror or 0)

    def mirror_counts():
        counts, ctrs = [], []
        for _ in range(NPKT):
            src = st["out_filled"] - D
            st["out_filled"] += 1
            if 0 <= src < len(inring):
                pos, n = inring[src]
                counts.append(min(n, maxf))
                ctrs.append((pos + lead) % 0x2000)
            else:
                if src >= 0:
                    st["mirror_miss"] += 1
                counts.append(0)
                ctrs.append(None)
        return counts, ctrs

    def fill_out(t):
        if mirror is not None:
            counts, ctrs = mirror_counts()
        else:
            counts, ctrs = next_counts(), None
        buf = bytearray()
        for i_, n in enumerate(counts):
            if ctrs and ctrs[i_] is not None:
                st["counter"] = ctrs[i_]
            pos = st["out_pos"]
            tt = (np.arange(n) + pos) / rate
            fr = np.zeros((n, nch + 1), np.float32)
            fr[:, 1 + k] = AMP * np.sin(2 * np.pi * F0 * tt)
            if offset is not None:            # a click every 0.1 s on channel k
                fr[:, 1 + k] = 0
                c0 = -(-pos // (rate // 10)) * (rate // 10)
                if c0 < pos + n and c0 > 0:
                    fr[c0 - pos, 1 + k] = 0.5
                    st.setdefault("clicks", []).append((st["counter"] + c0 - pos) % 0x2000)
                if st["in_next"] is not None:
                    st.setdefault("ahead", []).append((st["counter"] - st["in_next"]) % 0x2000)
            raw = bytearray(fr.tobytes())
            if n and counter_on:
                struct.pack_into("<I", raw, 0, st["counter"])
            st["counter"] = (st["counter"] + n) % 0x2000
            st["out_pos"] += n
            buf += raw
        lens = [n * (nch + 1) * 4 for n in counts]
        # python-libusb1 refuses empty packets: set them to 0 afterwards
        t.setIsochronous(0x01, bytes(buf) + bytes(4 * NPKT), callback=out_cb,
                         iso_transfer_length_list=[l or 1 for l in lens])
        if 0 in lens:
            descs = usb1.libusb1.get_iso_packet_list(t._USBTransfer__transfer)
            for d_, l in zip(descs, lens):
                d_.length = l

    held = []

    def out_cb(t):
        for s, _ in t.iterISO():
            st["out_pkts"] += 1
            if s != usb1.TRANSFER_COMPLETED:
                st["out_err"][s] += 1
                st.setdefault("out_err_t", []).append(time.time() - t0)
        if not st["running"]:
            return
        if stall and st.get("stall_at") is None and time.time() - t0 > 1.0:
            st["stall_at"] = time.time()
        if stall and st.get("stall_at") and time.time() - st["stall_at"] < 0.02:
            held.append(t)           # starve the device of OUT data
            return
        fill_out(t)
        t.submit()

    def in_cb(t):
        for s, data in t.iterISO():
            st["in_pkts"] += 1
            if s != usb1.TRANSFER_COMPLETED or len(data) < 4:
                if s != usb1.TRANSFER_COMPLETED:
                    st["in_err"][s] += 1
                n0 = round(nominal)
                inring.append((st["in_last_end"], n0))
                st["in_last_end"] = (st["in_last_end"] + n0) % 0x2000
                if s != usb1.TRANSFER_COMPLETED:
                    continue
            st["sizes"][len(data)] += 1
            n = (len(data) - 4) // (nch * 4) if len(data) >= 4 else 0
            if st["skip"]:           # an OUT packet already went out for it
                st["skip"] -= 1
            else:
                fb.append(n)
            if len(data) < 4:
                continue
            hdr = struct.unpack_from("<I", data, 0)[0]
            pos = (hdr >> 5) & 0x1fff
            inring.append((pos, n))
            st["in_last_end"] = (pos + n) % 0x2000
            st["in_next"] = (pos + n) % 0x2000
            if n and offset is not None:
                st.setdefault("in_pos_log", []).append((pos, n))
                st.setdefault("in_t", []).extend([time.time() - t0] * n)
            st.setdefault("hdrlog", []).append((time.time(), hdr, st["counter"]))
            if len(st["hdrs"]) < 64:
                st["hdrs"].append(struct.unpack_from("<I", data, 0)[0])
            if n:
                smp = np.frombuffer(bytes(data[4:4 + n * nch * 4]), np.float32)
                cap.append(smp.reshape(nch, n).T)   # channel-major -> frames
            st["in_frames"] += n
        if st["running"]:
            t.setIsochronous(0x82, in_mps * NPKT, callback=in_cb,
                             iso_transfer_length_list=[in_mps] * NPKT)
            t.submit()

    t0 = time.time()
    xfers = []
    for _ in range(NXFER):
        t = h.getTransfer(iso_packets=NPKT)
        t.setIsochronous(0x82, in_mps * NPKT, callback=in_cb,
                         iso_transfer_length_list=[in_mps] * NPKT)
        t.submit()
        xfers.append(t)
        if mirror is not None:                # OUT alongside, as the driver does
            t = h.getTransfer(iso_packets=NPKT)
            fill_out(t)
            t.submit()
            xfers.append(t)
    if mirror is not None:
        pass
    elif offset is not None:                    # wait for the device position
        while st["in_next"] is None and time.time() - t0 < 1.0:
            ctx.handleEventsTimeout(0.001)
        st["counter"] = (st["in_next"] + offset) % 0x2000
    fb.clear()
    st["skip"] = NXFER * NPKT                 # the first OUT packets use nominal counts
    if not in_only and mirror is None:
        for _ in range(NXFER):
            t = h.getTransfer(iso_packets=NPKT)
            fill_out(t)
            t.submit()
            xfers.append(t)

    while time.time() - t0 < 2.0:
        ctx.handleEventsTimeout(0.002)
        if held and time.time() - st["stall_at"] >= 0.02:
            while held:              # end of the stall
                h_ = held.pop(0)
                fill_out(h_)
                h_.submit()
    st["running"] = False
    for t in xfers:
        try:
            t.cancel()
        except usb1.USBError:
            pass
    t1 = time.time()
    while any(t.isSubmitted() for t in xfers) and time.time() - t1 < 1:
        ctx.handleEventsTimeout(0.05)
    w(h, 0x13, 0xffff, 0xc000)
    w(h, 0x15, 0, k)
    status = r(h, 0x11)
    h.setInterfaceAltSetting(0, 0)
    h.releaseInterface(0)

    el = 2.0
    print(f"rate {rate} alt {alt} {'fixed' if fixed else 'implicit-fb'} counter={'on' if counter_on else 'off'}"
          f"{' IN-only' if in_only else ''}: IN {st['in_frames'] / el:.0f} frames/s over {st['in_pkts']} pkts,"
          f" OUT {st['out_pos'] / el:.0f} frames/s over {st['out_pkts']} pkts, status {status:#010x}")
    print(f"  IN sizes {dict(st['sizes'].most_common(4))}, IN errors {dict(st['in_err'])}, OUT errors {dict(st['out_err'])}")
    print(f"  IN headers {[hex(x) for x in st['hdrs'][:8]]}")
    if offset is not None and cap and st.get("clicks"):
        allx = np.concatenate(cap).astype(np.float64)[:, k]
        devpos = np.concatenate([(p + np.arange(n)) % 0x2000 for p, n in st["in_pos_log"]])
        hits = np.flatnonzero(np.abs(allx) > 0.25)
        got = [int(devpos[i]) for i in hits]
        sent = st["clicks"]
        # every captured click: lag to the nearest earlier sent counter value
        seq = [min((g - c) % 0x2000 for c in sent) for g in got]
        lags = collections.Counter(seq)
        tt = [round(st["in_t"][i], 3) for i in hits]
        print("  lag sequence:", list(zip(tt, seq)))
        slips = []; prev_n = None
        for (tm, hd, _) in st["hdrlog"]:
            nn = hd & 0x1f
            if nn not in (0, round(nominal)) and nominal == int(nominal):
                slips.append((round(tm - t0, 3), nn))
        print("  IN packets with unusual frame counts:", slips[:10], " mirror misses:", st["mirror_miss"])
        ah = st.get("ahead", [])
        print(f"  offset {offset}: {len(sent)} clicks sent, {len(got)} captured; lag (capture pos - counter):"
              f" {lags.most_common(4)}; counter ahead of IN pos at fill: min {min(ah)} max {max(ah)}"
              f" first {ah[:3]} last {ah[-3:]}")
    if os.environ.get("HDRDUMP"):
        import pickle
        pickle.dump(st["hdrlog"], open(os.environ["HDRDUMP"], "wb"))
    if cap and offset is None:
        allx = np.concatenate(cap).astype(np.float64)
        blk = rate // 100
        nb = len(allx) // blk
        on = 20 * np.log10(np.sqrt((allx[:nb * blk, k].reshape(nb, blk) ** 2).mean(1)) + 1e-15) > -40
        print(f"  tone present in {on[20:95].mean()*100:.0f}% of 0.2-0.95 s and {on[120:190].mean()*100:.0f}% of 1.2-1.9 s")
        x = allx[rate // 2:]
        db = 20 * np.log10(np.sqrt((x ** 2).mean(axis=0)) + 1e-15)
        print("  capture dBFS:", " ".join(f"{i}:{v:.0f}" for i, v in enumerate(db)))
        if len(x) > 1000:
            spec = np.abs(np.fft.rfft(x[:, k] * np.hanning(len(x))))
            print(f"  ch {k} peak {np.argmax(spec) * rate / len(x):.1f} Hz, expect {F0} at {20*np.log10(AMP/np.sqrt(2)):.1f} dBFS")


if __name__ == "__main__":
    main()
