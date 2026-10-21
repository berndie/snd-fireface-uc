#!/usr/bin/env python3
"""Read-only probe of an RME Fireface UC (vendor IN requests only)."""
import usb.core

PC_PID, MAC_PID = 0x3fc5, 0x3fc6

def main():
    dev = usb.core.find(idVendor=0x0424, idProduct=PC_PID) or \
          usb.core.find(idVendor=0x0424, idProduct=MAC_PID)
    if dev is None:
        raise SystemExit("Fireface UC not found")
    print(f"found {dev.idProduct:#06x} ({'PC' if dev.idProduct == PC_PID else 'Mac'} mode)")
    for req, length in [(0x1c, 4), (0x11, 4), (0x11, 16), (0x10, 4), (0x19, 4)]:
        try:
            data = bytes(dev.ctrl_transfer(0xc0, req, 0, 0, length, timeout=500))
            words = [int.from_bytes(data[i:i+4], 'little') for i in range(0, len(data), 4)]
            print(f"req {req:#04x} len {length:2}: {data.hex(' ')}  words={[hex(w) for w in words]}")
        except usb.core.USBError as e:
            print(f"req {req:#04x} len {length:2}: error {e}")

if __name__ == "__main__":
    main()
