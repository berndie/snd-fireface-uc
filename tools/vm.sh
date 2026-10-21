#!/bin/bash
# Test VM for the kernel driver: an Ubuntu 22.04 guest running the same
# 6.8 kernel as the host, with the Fireface UC passed through over USB
# (in PC or Mac mode, whichever it is in).
# A driver crash only takes down the VM.
#
#   tools/vm.sh start     boot the VM in the background (needs /dev/kvm)
#   tools/vm.sh ssh CMD   run a command in the VM
#   tools/vm.sh sync      copy driver/ and tools/ into the VM (~/ffuc)
#   tools/vm.sh stop      power the VM off
#   tools/vm.sh log       show the serial console log
#   tools/vm.sh restart   restart the VM (needed after the UC re-enumerates)
#
# VM_TRACE=1 logs QEMU's usb_host_* trace events to $VM_DIR/usb-trace.log.
#
# VM_DIR must contain disk.qcow2 and the id_ed25519 key used to
# provision it (see the cloud-init notes in README.md).
set -euo pipefail

VM_DIR=${VM_DIR:?set VM_DIR to the directory holding disk.qcow2}
REPO=$(cd "$(dirname "$0")/.." && pwd)
PORT=${VM_SSH_PORT:-2222}
MON_PORT=${VM_MONITOR_PORT:-4444}
SSH=(ssh -q -i "$VM_DIR/id_ed25519" -p "$PORT" -o StrictHostKeyChecking=no
     -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 ubuntu@127.0.0.1)

case "${1:-}" in
start)
	sudo qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 \
		-display none -daemonize -pidfile "$VM_DIR/qemu.pid" \
		-serial "file:$VM_DIR/console.log" \
		${VM_TRACE:+-trace "enable=usb_host_*,file=$VM_DIR/usb-trace.log"} \
		-monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
		-drive "file=$VM_DIR/disk.qcow2,if=virtio" \
		-netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22" \
		-device virtio-net-pci,netdev=n0 \
		-device qemu-xhci,id=xhci \
		-device usb-host,id=ffuc,bus=xhci.0,vendorid=0x0424,productid=0x3fc5 \
		-device usb-host,id=ffucmac,bus=xhci.0,vendorid=0x2a39,productid=0x3fc6,isobufs=${VM_ISOBUFS:-32},isobsize=${VM_ISOBSIZE:-64}
	for _ in $(seq 60); do
		"${SSH[@]}" true 2>/dev/null && { echo "VM up"; exit 0; }
		sleep 2
	done
	echo "VM did not come up, see $VM_DIR/console.log" >&2
	exit 1
	;;
ssh)
	shift
	exec "${SSH[@]}" "$@"
	;;
sync)
	rsync -a --delete -e "${SSH[*]:0:${#SSH[@]}-1}" \
		--exclude .venv --exclude __pycache__ --exclude '*.o' --exclude '*.ko' \
		"$REPO/driver" "$REPO/tools" "${SSH[-1]}:ffuc/"
	;;
stop)
	"${SSH[@]}" sudo poweroff 2>/dev/null || true
	for _ in $(seq 30); do
		[ -e "$VM_DIR/qemu.pid" ] && sudo kill -0 "$(sudo cat "$VM_DIR/qemu.pid")" 2>/dev/null || exit 0
		sleep 1
	done
	sudo kill "$(sudo cat "$VM_DIR/qemu.pid")"
	;;
log)
	sudo tail -n "${2:-50}" "$VM_DIR/console.log"
	;;
restart)
	# Needed whenever the Fireface UC re-enumerates (USB reset, replug, or
	# after using it from the container): QEMU's libusb cannot see new
	# devices inside a container, so only a fresh QEMU picks it up again.
	"$0" stop
	exec "$0" start
	;;
*)
	sed -n '2,16p' "$0"
	exit 1
	;;
esac
