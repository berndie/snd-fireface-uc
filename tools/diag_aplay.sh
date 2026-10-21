#!/bin/bash
# Duplex timeline diagnostic with plain aplay/arecord (no PortAudio).
# Plays a -30 dBFS tone on channel 0 with the hardware loopback on output 0
# and reports every 10 ms block of the capture in which it is missing.
#
#   diag_aplay.sh [seconds] [runs]
#
# LOOP_ALL=1 enables the loopback on every output instead, and prints the
# per-channel levels after a dropout to show where the tone went.
set -euo pipefail
SECS=${1:-10}
RUNS=${2:-3}
RATE=48000
CH=18
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

python3 - "$DIR/tone.raw" "$SECS" <<'EOF'
import sys, numpy as np
rate, ch, secs = 48000, 18, float(sys.argv[2])
t = np.arange(int(rate * secs)) / rate
buf = np.zeros((len(t), ch), np.float32)
buf[:, 0] = 0.0316 * np.sin(2 * np.pi * 997 * t)
buf.tofile(sys.argv[1])
EOF

loop() {
	python3 -c "
import usb.core
d = (usb.core.find(idVendor=0x0424, idProduct=0x3fc5) or usb.core.find(idVendor=0x2a39, idProduct=0x3fc6))
for o in (range($CH) if ${LOOP_ALL:-0} else [0]):
    d.ctrl_transfer(0x40, 0x15, $1, o, None)"
}

for run in $(seq "$RUNS"); do
	loop 1
	arecord -q -D hw:FirefaceUC -c $CH -f FLOAT_LE -r $RATE -d $((SECS + 1)) -t raw "$DIR/in.raw" &
	sleep 0.2
	aplay -q -D hw:FirefaceUC -c $CH -f FLOAT_LE -r $RATE -t raw "$DIR/tone.raw"
	wait
	loop 0
	python3 - "$DIR/in.raw" "$run" <<'EOF'
import sys, numpy as np
allc = np.fromfile(sys.argv[1], np.float32).reshape(-1, 18).astype(np.float64)
x = allc[:, 0]
blk = 480
n = len(x) // blk
db = 20 * np.log10(np.sqrt((x[:n * blk].reshape(n, blk) ** 2).mean(1)) + 1e-15)
on = np.flatnonzero(db > -40)
if not len(on):
    print(f"run {sys.argv[2]}: tone never seen"); sys.exit()
a, b = on[0], on[-1]
miss = [i for i in range(a, b) if db[i] <= -40]
print(f"run {sys.argv[2]}: tone {a*10}-{b*10} ms, {len(miss)} missing blocks inside"
      + (f", first at {miss[0]*10} ms, levels {np.round(db[miss[:5]], 0).tolist()}" if miss else ""))
stop = b + 1
if stop * blk < len(x) - 150 * blk:          # tone ended well before the capture did
    seg = allc[stop * blk:(stop + 100) * blk]
    lv = 20 * np.log10(np.sqrt((seg ** 2).mean(0)) + 1e-15)
    print("  after tone vanished, per-channel dBFS:", " ".join(f"{i}:{v:.0f}" for i, v in enumerate(lv)))
EOF
done
