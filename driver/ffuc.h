/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RME Fireface UC - USB audio driver (PC and Mac mode)
 *
 * See docs/PROTOCOL.md for the USB protocol this driver speaks.
 *
 * Big picture:
 *
 *  - The unit has two USB personalities, chosen on its front panel.  Both
 *    stream float32 audio on interface 0 and share every vendor request,
 *    register and mixer address; only the transport differs, so all of the
 *    driver except the URB layer is common to both.
 *
 *  - In PC mode (0424:3fc5) interface 0 carries the audio on INTERRUPT
 *    endpoints.  The alternate setting selects a bandwidth class:
 *
 *      alt 1   32 - 48 kHz   18 ch   EP 0x01 OUT / 0x82 IN, 576 B packets
 *      alt 2   64 - 96 kHz   14 ch   EP 0x01 OUT / 0x82 IN, 896 B packets
 *      alt 3  128 - 192 kHz  12 ch   EP 0x01+0x03 OUT / 0x82+0x84 IN,
 *                                    6 ch per endpoint, 768 B packets
 *
 *    Every frame is one float32 LE word per channel, interleaved, no
 *    header.  The device paces both directions from its own clock, and
 *    only streams IN while OUT is being fed, so the "engine" always runs
 *    every endpoint of both directions together.
 *
 *  - In Mac mode (2a39:3fc6) the same alternate settings and channel counts
 *    use one ISOCHRONOUS endpoint pair (0x01 OUT / 0x82 IN).  The device
 *    decides how many frames each IN packet carries (implicit feedback):
 *    an IN packet is a 4-byte header plus N frames stored channel by
 *    channel, and the driver sends OUT packets with the same frame counts,
 *    each frame being a header word plus the samples.  The IN header holds
 *    the device's sample position (bits 5-17) and the frame count (bits
 *    0-4); the header word of an OUT packet's first frame tells the device
 *    where to play the samples, and must be just ahead of the position the
 *    device has reached when the packet arrives (see ffuc_iso_playback_fill).
 *
 *  - The engine is started from the first prepare() and stopped from the
 *    last hw_free(); trigger() only flips a per-direction running flag,
 *    so the URB callbacks know whether to copy PCM data or silence.
 *
 *  - The URB callbacks copy between the URB buffers and the ALSA ring
 *    buffer under the substream lock, converting to/from S32_LE when
 *    the application did not ask for FLOAT_LE.
 *
 *  - PC mode only: if OUT data stops arriving for about 5 ms, the device latches all
 *    outputs to silence until the alternate setting is selected again.
 *    It flags this in the status ticker on interface 5, so the driver
 *    watches that endpoint while streaming and restarts the engine (and
 *    reports an XRUN) when the flag shows up.
 */
#ifndef FFUC_H
#define FFUC_H

#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <sound/core.h>
#include <sound/pcm.h>

#define FFUC_VENDOR_ID		0x0424	/* the SMSC USB controller */
#define FFUC_VENDOR_RME		0x2a39
#define FFUC_PRODUCT_PC		0x3fc5	/* "Fireface UC Win" */
#define FFUC_PRODUCT_MAC	0x3fc6	/* "Fireface UC Mac" */

/* usb_device_id.driver_info */
#define FFUC_TRANSPORT_INT	0	/* PC mode */
#define FFUC_TRANSPORT_ISO	1	/* Mac mode */

#define FFUC_IFACE		0
#define FFUC_STATUS_IFACE	5
#define FFUC_STATUS_EP		0x8b	/* interrupt IN, 4 bytes */
#define FFUC_STATUS_LEN		4
#define FFUC_STATUS_OUT_LATCHED	0x80	/* in byte 3: outputs latched silent */
#define FFUC_STATUS_GRACE_MS	20	/* ignore the flag right after a start */

#define FFUC_MAX_EPS		2	/* endpoints per direction (alt 3) */
#define FFUC_MAX_URBS		32
#define FFUC_MAX_CHANNELS	18
#define FFUC_SAMPLE_BYTES	4
#define FFUC_MAX_URB_ERRORS	16	/* consecutive, before giving up */
/* Frames always queued towards the device (at 1x rates); much less than
 * this and ordinary scheduling jitter latches the outputs.
 */
#define FFUC_MIN_QUEUE_FRAMES	64

/* Mac mode */
#define FFUC_MAX_ISO_PACKETS	8	/* per URB, one per 125 us microframe */
#define FFUC_ISO_IN_HEADER	4	/* bytes in front of every IN packet */
#define FFUC_ISO_COUNTER_MOD	0x2000	/* device positions wrap here */
#define FFUC_FB_SIZE		1024	/* IN packets remembered, power of 2 */
#define FFUC_ISO_FRAME_MASK	0x7ff	/* start frames compare modulo this + 1 */

/* Vendor requests (bmRequestType 0x40 OUT / 0xc0 IN, recipient device).
 *
 * Several OUT requests are masked register writes: wValue holds the new
 * bits and wIndex the mask of bits to change.  The firmware treats an IN
 * request with a non-zero wIndex the same way (a masked write of wValue),
 * so every read must be issued with wIndex = 0.
 */
#define FFUC_REQ_SETTINGS	0x10	/* masked: clock, rate family, digital */
#define FFUC_REQ_STATUS		0x11	/* read: status, byte 3 = rate index */
#define FFUC_REQ_MIXER		0x12	/* wValue gain, wIndex mixer address */
#define FFUC_REQ_MUTE		0x13	/* hardware mute of all outputs */
#define FFUC_REQ_UNMUTE		0x14
#define FFUC_REQ_LOOPBACK	0x15	/* wValue on/off, wIndex output */
#define FFUC_REQ_CHAN_INIT	0x16
#define FFUC_REQ_LEVELS		0x17	/* masked: levels, 48V, PAD, Hi-Z */
#define FFUC_REQ_PGA		0x1a	/* input gain / output attenuator */
#define FFUC_REQ_DDS		0x1b	/* sample-rate generator */
#define FFUC_REQ_REVISION	0x1c	/* read: firmware revision in bits 23..16 */
#define FFUC_REQ_ENABLE		0x20

/* FFUC_REQ_LEVELS register (mask 0x0fc0 covers the three level fields). */
#define FFUC_LEVELS_MASK	0x0fc0
#define FFUC_LINE_OUT_SHIFT	6
#define FFUC_LINE_IN_SHIFT	8
#define FFUC_PHONES_SHIFT	10

/* FFUC_REQ_SETTINGS register. */
#define FFUC_SETTINGS_MASK	0x46cf
#define FFUC_SETTINGS_FIXED	0x4200	/* always set by the vendor driver */
#define FFUC_CLOCK_MASK		0x0007
#define FFUC_SINGLE_SPEED	0x0040	/* word clock out at single speed */
#define FFUC_SPDIF_PRO		0x0080	/* coax out in AES (professional) */
#define FFUC_OPTICAL_SPDIF	0x0400	/* optical I/O is SPDIF, not ADAT */
#define FFUC_RATE_FAMILY_MASK	0x0030	/* 0 = 32k, 1 = 44.1k, 2 = 48k (<<4) */

/* Mixer addresses (FFUC_REQ_MIXER).  Each output owns a block of 36
 * sources: 18 hardware inputs followed by 18 playback channels.
 */
#define FFUC_MIX_OUTPUTS	18
#define FFUC_MIX_STRIDE		36
#define FFUC_MIX_PLAYBACK	18
#define FFUC_MIX_MASTER		992	/* + output */
#define FFUC_MIX_UNITY		0x2000	/* 0 dB, hardware-verified */

struct ffuc;

/* One USB bandwidth class (alternate setting). */
struct ffuc_mode {
	unsigned int alt;
	unsigned int neps;		/* endpoints per direction */
	unsigned int words;		/* channels (32-bit words) per endpoint */
	unsigned int pkt_frames;	/* PC: frames in one max-size packet */
	/* Mac mode only */
	unsigned int in_mps;		/* bytes per microframe, incl. mult */
	unsigned int out_mps;
	unsigned int max_frames;	/* most frames one packet can carry */
};

struct ffuc_urb {
	struct urb *urb;
	struct ffuc_ep *ep;
	void *buf;
	dma_addr_t dma;
};

struct ffuc_ep {
	struct ffuc *chip;
	int dir;			/* SNDRV_PCM_STREAM_* */
	unsigned int index;		/* 0 or 1, selects channel half in alt 3 */
	int next_start;			/* Mac mode: start frame expected next */
	struct ffuc_urb urbs[FFUC_MAX_URBS];
};

struct ffuc_stream {
	struct snd_pcm_substream *substream;
	/* Everything below is protected by the substream lock. */
	bool running;
	snd_pcm_uframes_t pos[FFUC_MAX_EPS];	/* frames moved per endpoint */
	snd_pcm_uframes_t hw_ptr;		/* min over the endpoints */
	snd_pcm_uframes_t period;		/* hw_ptr / period_size */
};

struct ffuc {
	struct usb_device *dev;
	struct usb_interface *intf;
	struct usb_interface *status_intf;	/* claimed, or NULL */
	struct snd_card *card;
	struct snd_pcm *pcm;

	struct mutex mutex;		/* engine state, rate, controls */
	int dev_idx;			/* slot in the index/id/enable arrays */
	bool disconnected;
	bool iso;			/* Mac mode (isochronous transport) */
	const struct ffuc_mode *modes;	/* the 1x/2x/4x table of this mode */

	/* engine */
	bool engine_running;		/* written under mutex, read in URB cb */
	bool engine_broken;		/* set by URB cb on fatal errors */
	unsigned int hw_params_mask;	/* directions with a configured rate */
	unsigned int prepared_mask;	/* directions holding the engine */
	unsigned int rate;
	const struct ffuc_mode *mode;
	unsigned int urb_frames;	/* per URB at the current mode */
	unsigned int base_urb_frames;	/* module parameter, at 1x rates */
	unsigned int nurbs;
	size_t urb_bytes_max;
	atomic_t urb_errors;

	/* Mac mode: implicit feedback and the OUT position */
	unsigned int iso_packets;	/* packets per URB */
	spinlock_t fb_lock;
	u32 fb[FFUC_FB_SIZE];		/* per IN packet: position << 5 | frames */
	u64 in_pkts, out_pkts;		/* packets since the engine started */
	unsigned int in_end;		/* position after the last IN packet */
	unsigned int fb_acc;		/* nominal-rate accumulator, 1/8000 frames */
	unsigned int fb_delay;		/* OUT packet k mirrors IN packet k - this */
	unsigned int out_lead;		/* frames from IN position to OUT position */
	bool iso_frames_ok;		/* the HCD reports contiguous start frames */

	/* output-latch watchdog (status ticker on interface 5) */
	struct urb *status_urb;
	u8 *status_buf;
	unsigned long status_armed_at;	/* jiffies, end of the grace period */
	struct work_struct recover_work;
	unsigned int recoveries;

	struct ffuc_ep eps[2][FFUC_MAX_EPS];
	struct ffuc_stream streams[2];

	/* control state mirrors */
	u16 levels;			/* FFUC_REQ_LEVELS bits under the mask */
	u16 settings;			/* FFUC_REQ_SETTINGS bits under the mask */
};

/* ffuc.c */
int ffuc_write(struct ffuc *chip, u8 req, u16 val, u16 idx);
int ffuc_read(struct ffuc *chip, u8 req, u32 *val);
unsigned int ffuc_rate_from_index(unsigned int idx);

/* ffuc-ctl.c */
int ffuc_settings_load(struct ffuc *chip);
int ffuc_settings_apply(struct ffuc *chip);
int ffuc_mixer_reset(struct ffuc *chip);
int ffuc_create_controls(struct ffuc *chip);

#endif /* FFUC_H */
