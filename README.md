# snd-fireface-uc — Linux ALSA driver for the RME Fireface UC

A kernel driver for the RME Fireface UC, in **PC mode** (USB ID `0424:3fc5`,
shown by `lsusb` as "Fireface UC Win") and **Mac mode** (`2a39:3fc6`,
"Fireface UC Mac"). The driver detects which mode the unit is in. It
provides a normal ALSA sound card, so ALSA applications, PulseAudio/PipeWire,
PortAudio and Python's `sounddevice` all work with it.

Status: early. Tested in a KVM guest running the host's kernel (6.8.0-138)
with the unit passed through: full-duplex streaming at all nine rates in both
formats, verified sample-exact through the unit's hardware loopback, plus
start/stop, control, PulseAudio and hot-unplug tests (PC mode; Mac mode
passes the loopback test at the integer-multiple rates and the stress test
in the VM). It has not yet run on bare metal.

| | |
|---|---|
| Rates | 32, 44.1, 48, 64, 88.2, 96, 128, 176.4, 192 kHz |
| Channels | 18 in / 18 out at ≤ 48 kHz, 14 at ≤ 96 kHz, 12 at ≤ 192 kHz (AN 1–8, SPDIF, ADAT) |
| Formats | `FLOAT_LE` (native) and `S32_LE` |
| Latency | ≈ 5–6 ms round trip at 48 kHz with 32–64-frame application buffers (digital loopback, measured); tunable |
| Controls | Line Out / Phones / Line In level (incl. Hi Gain), clock source, optical and SPDIF format, word clock single speed, current sample rate |

To switch modes: unplug USB, then quickly double-click the front control
knob until the display shows the mode you want. PC mode is the
better-tested one (see "Mac mode" below).

## Build and load

You need the kernel headers for the running kernel (`linux-headers-$(uname -r)`).

```sh
cd driver
make
sudo modprobe snd-pcm
sudo insmod snd-fireface-uc.ko
```

Check it came up:

```sh
sudo dmesg | tail          # "RME Fireface UC (PC mode, firmware 127) ..., card N"
aplay -l                   # card N: FirefaceUC [Fireface UC]
amixer -c FirefaceUC       # the controls below
```

To unload: `sudo rmmod snd_fireface_uc`.

If `insmod` fails with "Key was rejected by service", Secure Boot is
enforcing module signatures; install through DKMS, which signs with your
machine-owner key (MOK), or sign the module by hand.

### Install permanently (DKMS)

```sh
sudo cp -r driver /usr/src/snd-fireface-uc-0.1
sudo dkms install snd-fireface-uc/0.1
```

The module then loads automatically when the Fireface UC is plugged in.

## Module parameters

| parameter | default | meaning |
|-----------|---------|---------|
| `urb_frames` | 32 | frames per USB transfer at 32–48 kHz (multiple of 8, 8–256; doubled at 2x and quadrupled at 4x rates). Smaller means lower latency and more CPU. |
| `nurbs` | 4 | transfers in flight per endpoint (2–32). More means more robust and more latency. |
| `fixed_rate` | 0 | only allow this sample rate (e.g. `48000`); 0 = any. Writable at runtime via `/sys/module/snd_fireface_uc/parameters/fixed_rate`. |
| `reset_mixer` | 1 | at load, route playback *n* → output *n* at 0 dB, turn off input monitoring and loopback. |
| `mac_out_offset` | 0 | Mac mode only: frames added to the playback position sent to the unit (see "Mac mode"). Writable at runtime; applies from the next stream start. |

Example: `sudo insmod snd-fireface-uc.ko urb_frames=16 nurbs=4 fixed_rate=48000`.

## Controls

| control | values |
|---------|--------|
| `Line Out Level` | Hi Gain, +4 dBu, −10 dBV |
| `Phones Level` | Hi Gain, +4 dBu, −10 dBV |
| `Line In Level` | Lo Gain, +4 dBu, −10 dBV |
| `Clock Source` | Internal, SPDIF Coaxial, Optical, Word Clock |
| `Optical Output Format` | ADAT, SPDIF |
| `SPDIF Output Format` | Consumer, Professional (AES) |
| `Word Clock Single Speed Switch` | off/on |
| `Sample Rate` | read-only, the rate the unit currently runs at |

```sh
amixer -c FirefaceUC sset 'Line Out Level' 'Hi Gain'
amixer -c FirefaceUC sset -- 'Phones Level' '-10 dBV'   # "--" because of the leading "-"
```

The sample rate is set by the application that opens the device, as usual
for ALSA. Playback and capture share one clock, so while one direction is
open at some rate the other is held to the same rate. To pin the rate for
everything, use `fixed_rate`.

**With PulseAudio, set `fixed_rate=48000`.** Otherwise PulseAudio can move
its source and sink to different rates while one of them is suspended, and
then fails to resume with "couldn't restore original sample settings"
(`paplay` hangs). Applications are resampled by the sound server as usual:

```sh
echo 'options snd-fireface-uc fixed_rate=48000' | sudo tee /etc/modprobe.d/snd-fireface-uc.conf
```

## Using it

```sh
speaker-test -D hw:FirefaceUC -c 2 -F FLOAT_LE -r 48000
arecord -D hw:FirefaceUC -c 18 -f FLOAT_LE -r 48000 -d 5 in.wav
```

The hardware accepts any channel count from 1 up to the maximum; channel *n*
of the application maps to device channel *n* (AN 1, AN 2, …). Through
`plughw:FirefaceUC` or PulseAudio/PipeWire, any format works.

Python:

```python
import sounddevice as sd
sd.default.device = "Fireface UC"
sd.default.samplerate = 48000
rec = sd.playrec(signal, channels=2, dtype="float32")
```

## Underrun recovery

If the USB stream to the unit stalls for about 5 ms or more (system load, a busy
USB bus), the Fireface UC silences all of its outputs until the stream is
re-initialised. The driver watches for this on the unit's status endpoint
and restarts the stream within a few milliseconds. Applications see one
XRUN and carry on; the kernel log shows
`stream interrupted (output underrun or USB error), restarting (N)`.

## Mac mode

In Mac mode the audio travels over isochronous endpoints. Each playback
packet tells the unit at which sample position to play it, and that
position has to stay just ahead of where the unit is. If it is too far
ahead (more than about 1 ms), the unit places the data unpredictably. The
driver does what the macOS driver does: playback packet *k* copies the
frame count and position of capture packet *k − D*, plus the time those
*D* microframes take, plus 12/24/48 frames (at 1x/2x/4x rates). This was
checked against the hardware with a userspace (libusb) model of the same
scheme (`tools/ffuc_mac.py --mirror=N`). It cannot be checked inside the
test VM, because QEMU's isochronous passthrough delays and batches packets
by tens of milliseconds. In the VM, Mac-mode round-trip latency is about
60–150 ms, and the 44.1/88.2/176.4 kHz rates lose level. Neither is
expected on bare metal; measure with `tools/click_lag.py`, which reports
whether the loopback lag stays constant. If it doesn't on your machine,
try `mac_out_offset` values between −32 and 32.

If the host controller restarts a transfer queue that ran dry (which breaks
the playback/capture pairing), the driver restarts the stream, as it does
for an underrun in PC mode.

## Testing without touching the host kernel

`tools/vm.sh` runs an Ubuntu 22.04 VM with the host's kernel version under
KVM, with the Fireface UC passed through over USB, so a driver bug can only
crash the VM. It needs `/dev/kvm` in the container (see
`.devcontainer/devcontainer.json`) and a disk provisioned with cloud-init
(kernel `6.8.0-*-generic` with headers, `build-essential`, `alsa-utils`,
`python3-usb`, `python3-numpy`, `libportaudio2`, `sounddevice`). Then:

```sh
export VM_DIR=/path/to/vm          # disk.qcow2 + id_ed25519
tools/vm.sh start
tools/vm.sh sync
tools/vm.sh ssh 'cd ffuc/driver && make && sudo modprobe snd-pcm && sudo insmod snd-fireface-uc.ko'
tools/vm.sh ssh 'sudo python3 ffuc/tools/test_alsa.py'   # loopback test, all rates/formats
tools/vm.sh ssh 'sudo ffuc/tools/stress.sh'               # lifecycle, controls, unplug
```

USB passthrough adds timing jitter, so expect occasional recovered underruns
inside the VM. Also, if the unit re-enumerates, QEMU cannot pick it up
again inside a container, and `tools/vm.sh restart` is needed.

## Layout

* `driver/` — the kernel module (`ffuc.c` streaming and PCM, `ffuc-ctl.c` controls)
* `docs/PROTOCOL.md` — the USB protocol as reverse-engineered and verified on hardware
* `tools/` — userspace probes used to work out the protocol (`ffuc_*.py`), and
  the driver tests: `test_alsa.py` (loopback, all rates and formats),
  `stress.sh` (lifecycle, controls, unplug), `latency.py`, `click_lag.py`
  (loopback lag stability), `diag_duplex.py` and
  `diag_aplay.sh` (dropout timelines), `vm.sh` (the test VM)

## Known issues and not done yet

* Not yet run on a bare-metal kernel, only in the KVM guest.
* Twice during development the unit dropped off the USB bus and re-enumerated
  by itself; this did not recur in a later 11-minute soak, and the cause is
  unknown. If it happens, `journalctl -k` on the host shows what the USB core
  saw.
* PipeWire was not tested (the test guest runs PulseAudio 15).
* Mac mode has only been tested in the VM, which cannot show its latency or
  timing behaviour (see "Mac mode").
* Mic gain / 48V / PAD / Hi-Z controls, the DSP mixer, MIDI and the
  level meters are not implemented.
* While no stream is running, the outputs are hardware-muted (as the vendor
  driver does), so the unit's own input monitoring is silent when idle.

Protocol background: <https://github.com/agfline/RME-Fireface-UC-Drivers>
(Mac mode), and the RME Babyface Pro proprietary-mode ALSA RFC on
linux-sound, which uses the same interrupt-endpoint scheme.
