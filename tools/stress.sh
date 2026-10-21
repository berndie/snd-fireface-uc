#!/bin/bash
# Lifecycle stress test for snd-fireface-uc (run inside the test VM as root).
#
#   1. start/stop cycles over rates, formats and directions
#   2. control writes, checked against the device registers
#   3. driver unbind while streaming (simulated unplug), then rebind
#
# Prints a summary; the kernel log is checked for warnings at the end.
set -uo pipefail
CARD=hw:FirefaceUC
fail=0
note() { echo "  $*"; }

dmesg -C

echo "1. start/stop cycles"
i=0
for rate in 44100 48000 96000 192000 32000 88200 176400; do
	case $rate in 32000|44100|48000) ch=18 ;; 88200|96000) ch=14 ;; *) ch=12 ;; esac
	for fmt in FLOAT_LE S32_LE; do
		i=$((i + 1))
		timeout 10 arecord -q -D $CARD -c $ch -f $fmt -r $rate -d 1 -t raw /dev/null &
		rec=$!
		timeout 10 aplay -q -D $CARD -c 2 -f $fmt -r $rate -t raw -d 1 /dev/zero
		p=$?
		wait $rec
		r=$?
		[ $p -eq 0 ] && [ $r -eq 0 ] || { note "cycle $i ($rate $fmt): aplay=$p arecord=$r"; fail=1; }
		# one direction alone, short
		timeout 5 aplay -q -D $CARD -c $ch -f $fmt -r $rate -t raw -s $((rate / 10)) /dev/zero || { note "playback-only $rate $fmt failed"; fail=1; }
		timeout 5 arecord -q -D $CARD -c 1 -f $fmt -r $rate -s $((rate / 10)) -t raw /dev/null || { note "capture-only $rate $fmt failed"; fail=1; }
	done
done
# mismatched rates: the second stream must be held to the running rate,
# without disturbing the first one
timeout 10 arecord -q -D $CARD -c 2 -f FLOAT_LE -r 48000 -d 2 -t raw /dev/null &
rec=$!
sleep 0.5
msg=$(timeout 5 aplay -D $CARD -c 2 -f FLOAT_LE -r 96000 -t raw -s 4800 /dev/zero 2>&1)
echo "$msg" | grep -q 'got = 48000Hz' && note "playback asking for 96k during 48k capture was held to 48k" ||
	{ note "rate lock not applied: $msg"; fail=1; }
wait $rec || { note "capture broke when playback asked for another rate"; fail=1; }
note "$i duplex cycles plus single-direction and rate-conflict runs done"

echo "2. controls"
reg() { python3 -c "
import usb.core
d = (usb.core.find(idVendor=0x0424, idProduct=0x3fc5) or usb.core.find(idVendor=0x2a39, idProduct=0x3fc6))
print(hex(int.from_bytes(bytes(d.ctrl_transfer(0xc0, $1, 0, 0, 4)), 'little')))"; }
check_ctl() {	# control value request mask expected
	amixer -q -c FirefaceUC sset -- "$1" "$2" || { note "amixer $1 $2 failed"; fail=1; return; }
	got=$(( $(reg "$3") & $4 ))
	[ $got -eq $(( $5 )) ] && note "$1 = $2 -> reg $(printf %#x $got) ok" ||
		{ note "$1 = $2 -> reg $(printf %#x $got), expected $5"; fail=1; }
}
check_ctl 'Line Out Level' 'Hi Gain' 0x17 0x00c0 0x0080
check_ctl 'Line Out Level' '-10 dBV' 0x17 0x00c0 0x0040
check_ctl 'Line Out Level' '+4 dBu' 0x17 0x00c0 0x00c0
check_ctl 'Phones Level' 'Hi Gain' 0x17 0x0c00 0x0400
check_ctl 'Phones Level' '-10 dBV' 0x17 0x0c00 0x0800
check_ctl 'Line In Level' 'Lo Gain' 0x17 0x0300 0x0000
check_ctl 'Line In Level' '+4 dBu' 0x17 0x0300 0x0100
check_ctl 'Optical Output Format' 'SPDIF' 0x10 0x0400 0x0400
check_ctl 'Optical Output Format' 'ADAT' 0x10 0x0400 0x0000
check_ctl 'SPDIF Output Format' 'Professional' 0x10 0x0080 0x0080
check_ctl 'SPDIF Output Format' 'Consumer' 0x10 0x0080 0x0000
check_ctl 'Word Clock Single Speed' 'on' 0x10 0x0040 0x0040
check_ctl 'Word Clock Single Speed' 'off' 0x10 0x0040 0x0000
check_ctl 'Clock Source' 'Internal' 0x10 0x0007 0x0001
rate=$(amixer -c FirefaceUC cget name='Sample Rate' | sed -n 's/.*: values=//p')
note "Sample Rate control reads $rate"

echo "3. unbind while streaming, then rebind"
intf=$(basename "$(ls -d /sys/bus/usb/drivers/snd-fireface-uc/*:1.0 | head -1)")
timeout 10 aplay -q -D $CARD -c 2 -f FLOAT_LE -r 48000 -t raw -d 4 /dev/zero 2>/dev/null &
ap=$!
timeout 10 arecord -q -D $CARD -c 18 -f FLOAT_LE -r 48000 -d 4 -t raw /dev/null 2>/dev/null &
ar=$!
sleep 1
echo "$intf" > /sys/bus/usb/drivers/snd-fireface-uc/unbind
wait $ap; ap_rc=$?
wait $ar; ar_rc=$?
note "after unbind: aplay exit $ap_rc, arecord exit $ar_rc (non-zero expected, no hang)"
[ $ap_rc -eq 124 ] || [ $ar_rc -eq 124 ] && { note "an app hung after unbind"; fail=1; }
echo "$intf" > /sys/bus/usb/drivers/snd-fireface-uc/bind
sleep 1
timeout 10 arecord -q -D $CARD -c 18 -f FLOAT_LE -r 48000 -d 1 -t raw /dev/null &&
	note "capture works after rebind" || { note "capture after rebind failed"; fail=1; }

echo "kernel log:"
dmesg | grep -vE 'engine st|registered new|deregistering' | sed 's/^/  /'
if dmesg | grep -qiE 'BUG|WARNING|Oops|UBSAN|refcount|use-after|general protection'; then
	note "kernel warnings found"; fail=1
fi
[ $fail -eq 0 ] && echo "STRESS: PASS" || echo "STRESS: FAIL"
exit $fail
