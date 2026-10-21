# RME Fireface UC — USB protocol

Everything here was measured on a Fireface UC, firmware 127 (`0x1c` revision
word `0x007f0103`), in PC mode (USB PID `0x3fc5`, product string
"Fireface UC Win (<serial>)"). Items marked *(Mac capture)* come from
the agfline reverse-engineering captures of the macOS driver (firmware 118)
and were cross-checked against PC mode where noted.

## Modes

The front-panel mode switch (double-click the control knob while USB is
unplugged) selects one of two USB personalities:

| VID:PID     | Mode | Audio transport |
|-------------|------|-----------------|
| `0424:3fc5` | PC ("Fireface UC Win") | interrupt endpoints on interface 0 |
| `2a39:3fc6` | Mac ("Fireface UC Mac") | isochronous endpoints on interface 0 |

PC mode uses vendor ID `0x0424` (the SMSC USB controller); Mac mode uses
RME's own `0x2a39` (firmware 127; the RE notes assumed `0424:3fc6`). The
vendor requests, registers, alternate settings, rates and channel counts
are the same in both modes; only the audio transport differs.

## Interfaces (PC mode)

| IF | Endpoints | Use |
|----|-----------|-----|
| 0  | 0x01 OUT, 0x82 IN, 0x03 OUT, 0x84 IN | audio (see below) |
| 1  | 0x85 bulk IN | unknown (likely level meters) |
| 2  | 0x86 bulk IN, 0x07 bulk OUT | probably MIDI |
| 3  | 0x88 bulk IN, 0x09 bulk OUT | unknown |
| 4  | 0x8a bulk IN | unknown |
| 5  | 0x8b interrupt IN, 4 B | unknown (status ticker) |

## Audio streaming

The alternate setting of interface 0 selects a bandwidth class:

| alt | rates            | channels | endpoints (OUT / IN)        | wMaxPacketSize | frames / packet |
|-----|------------------|----------|-----------------------------|----------------|-----------------|
| 0   | idle             | –        | –                           | 64             | –               |
| 1   | 32, 44.1, 48 kHz | 18       | 0x01 / 0x82                 | 576            | 8               |
| 2   | 64, 88.2, 96 kHz | 14       | 0x01 / 0x82                 | 896            | 16              |
| 3   | 128, 176.4, 192 kHz | 12    | 0x01+0x03 / 0x82+0x84, 6 ch each | 768       | 32              |

All endpoints used for audio are **interrupt** endpoints with bInterval 1
(one transaction per 125 µs microframe). In alts 0–2, EP 0x03/0x84 are bulk
and not used for audio.

* **Frame format:** one IEEE-754 float32 little-endian word per channel,
  channels interleaved, no header or counter. Full scale is ±1.0.
* **Channel order** (1x): AN 1–8, SPDIF L/R, ADAT 1–8. At 2x the ADAT block
  shrinks to 4 channels (S/MUX), at 4x to 2. In alt 3 channels 1–6 travel on
  EP 0x01/0x82 and channels 7–12 on EP 0x03/0x84.
* **Clocking:** the device paces both directions. IN packets are always
  full-size; the endpoint NAKs until the next packet is ready. OUT is accepted
  at exactly the sample rate (back-pressure through NAKs). Measured at 48 kHz:
  3.454 MB/s in each direction = 48000 × 18 × 4.
* **IN only flows while OUT is being fed.** Reading 0x82 without
  writing 0x01 times out. A driver must therefore always run both directions.
* A transfer buffer that is not a multiple of wMaxPacketSize ends in
  `-EOVERFLOW` (babble) on IN.
* **Output latch.** If no OUT data arrives for about 5 ms (2–3 ms is
  tolerated), the device latches *all* outputs to digital silence. It keeps
  accepting OUT data at the normal rate and keeps streaming IN, so nothing
  on the bus shows it. Unmute, rate writes, the init sequence and
  CLEAR_FEATURE(HALT) on the endpoints do not clear it; selecting the
  alternate setting again does. The status ticker reports it (below).

## Status ticker (interface 5, EP 0x8b)

A 4-byte interrupt IN report, updated roughly every millisecond while
streaming:

| bytes | meaning |
|-------|---------|
| 0–1   | 16-bit counter, advances by 12 or 18 per report (purpose unknown) |
| 2     | 0 |
| 3     | flags: bit 6 toggles; **bit 7 = outputs latched** (see above), set within ~7 ms of the underrun, cleared by SET_INTERFACE on interface 0 |

Bit 7 tracked the latch exactly in 10 of 10 trials (stalls of 0–10 ms).

Verified via the hardware loopback (below): a −33.0 dBFS tone written to
playback channel *k* comes back on capture channel *k* at −33.0 dBFS, for
k = 0, 5, 8, 12 (48 kHz) and 3, 13 (96 kHz).

## Audio streaming (Mac mode)

Interface 0 again selects the bandwidth class with alts 1–3 (same rates and
channel counts as PC mode), but with one **isochronous** endpoint pair,
0x01 OUT / 0x82 IN, bInterval 1, in every alt:

| alt | channels | IN wMaxPacketSize | OUT wMaxPacketSize | max frames / packet |
|-----|----------|-------------------|--------------------|---------------------|
| 1   | 18       | 508               | 532                | 7                   |
| 2   | 14       | 732               | 780                | 13                  |
| 3   | 12       | 1208              | 1304               | 25                  |

* **IN packet:** a 4-byte header, then N frames stored **channel by channel**
  (all N samples of channel 1, then channel 2, …), float32 LE. Header bits
  0–4 = N, bits 5–17 = the device's sample position of the first frame
  (mod 0x2000, starts at 0 when the alt is selected), bits 18–31 = a phase
  value that wraps about every 0.84 s (when the device sends a 5/7-frame
  packet pair at 48 kHz); not needed. The device chooses N (6 per microframe
  at 48 kHz, 5/6 alternating at 44.1 kHz): implicit feedback.
* **OUT packet:** N frames of (1 header word + channels) float32 LE, frames
  interleaved. The header word of the first frame is the **position** the
  frame is to be played at (mod 0x2000); the other header words are 0.
  Without it the output is garbled. OUT packets carry the frame counts of
  the IN packets (zero-length packets are fine, e.g. before streaming).
* IN streams without OUT. There is no output latch as in PC mode: after an
  OUT gap the output glitches and recovers by itself.

### Where the OUT data lands

Measured with the hardware loopback on the host (libusb, `ffuc_mac.py
--offset/--mirror`): a click sent at position P comes back at capture
position P + 2 if the packet reaches the device shortly before the device
gets there. If it arrives late, it is played 64 frames later (128 at 4x),
and stays there: stable, just more latency. If it arrives more than about
1–1.3 ms early (≥ 80 frames at 48 kHz, ≥ 96 at 96 kHz, ≥ 256 at 192 kHz),
placement is unstable and jumps by multiples of 64 frames during the
stream. So the position must track the device closely.

The macOS driver (`initList3`, `performAudioEngineStart` in the RE repo's
FirefaceUSB.asm) does this structurally: OUT packet k mirrors IN packet
k − (nlists + 2)·P (P packets per list) — same frame count, position = that
IN packet's position + (nlists + 2)·P·rate/8000 + 2·max_frames − 2
(`cea0`/`cea4`; the extra safety offset `0x116ac` is 0 for the UC). The
positions are then continuous by construction, and the data is placed
2·max_frames − 2 = 12/24/48 frames ahead of where the device is.

## Vendor requests

All requests use recipient *device*, `bmRequestType` 0x40 (OUT, no data) or
0xc0 (IN, 4 bytes).

**Caution:** the firmware treats an IN request with non-zero wIndex as a
*write* (a masked write of wValue under the wIndex mask, as for the OUT
form). Reading `0x10` with wIndex 0x0fc0 clears bits 6–11 of the settings
register. Always read with wValue = wIndex = 0.

| bRequest | dir | wValue | wIndex | meaning |
|----------|-----|--------|--------|---------|
| 0x10 | OUT | bits | mask | settings register, masked write |
| 0x10 | IN  | – | 0 | settings register readback |
| 0x11 | IN  | – | 0 | status; byte 3 = current rate index |
| 0x12 | OUT | gain | address | DSP mixer gain, 0x2000 = 0 dB |
| 0x13 | OUT | 0xffff | 0xc000 | hardware mute all outputs |
| 0x14 | OUT | 0xffff | 0xc000 | hardware unmute |
| 0x15 | OUT | 0/1 | output | loopback output → same-numbered input |
| 0x16 | OUT | 1 | ch, ch+0x20 | channel init, sent for ch 0..17 at plug |
| 0x17 | OUT | bits | mask | levels register, masked write |
| 0x17 | IN  | – | 0 | levels register readback |
| 0x1a | OUT | value | index | input gain (index 0–3), output PGA (index 4–21) |
| 0x1b | OUT | dds hi | dds lo \| family | rate generator |
| 0x1c | IN  | – | 0 | firmware revision in bits 23..16 |
| 0x20 | OUT | 1 | 0 | global enable, sent at plug |

### Init (as sent by the vendor driver at plug, *(Mac capture)*, works in PC mode)

```
for ch in 0..17: 0x16 (1, ch); 0x16 (1, ch + 0x20)
0x10 (0x0800, 0xb800)
0x20 (0x0001, 0x0000)
```

### Sample rate

```
family = 0 for 32/64/128 kHz, 1 for 44.1/88.2/176.4 kHz, 2 for 48/96/192 kHz
dds    = 0x5f5e10000000 / rate, << 1 at 2x, << 2 at 4x (truncate to 32 bits)
0x1b (dds >> 16, (dds & 0xff00) | family)
0x10 (family << 4, 0x0030)
```

Select the alternate setting first; it sets the 1x/2x/4x multiplier.
Status (`0x11`) byte 3 then reads back the rate index:

| index | 0 | 1 | 2 | 4 | 5 | 6 | 8 | 9 | 10 |
|-------|---|---|---|---|---|---|---|---|----|
| rate  | 32k | 44.1k | 48k | 64k | 88.2k | 96k | 128k | 176.4k | 192k |

(Indices 3, 7 and 11 also mean 48k, 96k and 192k in the vendor's
`ufx_id_to_freq`.) Verified for all nine rates in PC mode.

### Stream start / stop *(Mac capture)*

start: set rate, `0x14 (0xffff, 0xc000)` (unmute) · stop: `0x13 (0xffff, 0xc000)` (mute).

### Settings register (`0x10`, mask `0x46cf`)

The vendor settings tool always writes `0x4200 | fields` under mask 0x46cf.

| bits | field | values |
|------|-------|--------|
| 2–0  | clock source | 1 internal, 2 SPDIF coax, 4 optical, 6 word clock |
| 5–4  | rate family (separate mask 0x0030) | see above |
| 6    | word clock out single speed | 1 = on |
| 7    | coax output format | 0 SPDIF consumer, 1 AES professional |
| 10   | optical I/O format | 0 ADAT, 1 SPDIF |
| 9, 14 | always 1 | |
| 11   | set by the init sequence | |

### Levels register (`0x17`, mask `0x0fc0` for the level fields)

| bits  | field     | Hi/Lo Gain | +4 dBu | −10 dBV |
|-------|-----------|------------|--------|---------|
| 7–6   | line out  | 10 (Hi)    | 11     | 01      |
| 9–8   | line in   | 00 (Lo)    | 01     | 11      |
| 11–10 | phones    | 01 (Hi)    | 11     | 10      |

The same register carries 48V (bit 0: mic 1, bit 1: mic 2), PAD (bit 3:
in 3, bit 2: in 4) and Hi-Z (bit 5: in 3, bit 4: in 4), each written with its
own single-bit mask *(Mac capture)*. Readback of the level bits was
verified in PC mode for every value in the table.

### DSP mixer (`0x12`)

Output *o* (0–17) owns addresses `36·o … 36·o + 35`: sources 0–17 are the
hardware inputs, 18–35 the playback channels. Output masters are at
`992 + o`. 0x2000 is 0 dB (verified: unity loopback). The vendor's
TotalMix sometimes writes addresses with bits 14–15 set (computed by
`CCardIOUC::SetGain`); these are not needed for plain routing.
