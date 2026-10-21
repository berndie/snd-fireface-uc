// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Fireface UC - USB audio driver (PC and Mac mode)
 *
 * Core: vendor requests and hardware init, the streaming engine (interrupt
 * URBs in PC mode, isochronous URBs in Mac mode), the ALSA PCM device and
 * the USB driver glue.  See ffuc.h for
 * the overall design and docs/PROTOCOL.md for the protocol.
 */
#include <linux/bitops.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "ffuc.h"

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;
static unsigned int urb_frames = 32;
static unsigned int nurbs = 4;
static unsigned int fixed_rate;
static bool reset_mixer = true;
static int mac_out_offset;

static DEFINE_MUTEX(devices_mutex);
static unsigned long devices_used;

static struct usb_driver ffuc_driver;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the Fireface UC sound card.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the Fireface UC sound card.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable the Fireface UC sound card.");
module_param(urb_frames, uint, 0444);
MODULE_PARM_DESC(urb_frames, "Frames per URB at 32-48 kHz, multiple of 8 (8..256, default 32); doubled at 2x and quadrupled at 4x rates.");
module_param(nurbs, uint, 0444);
MODULE_PARM_DESC(nurbs, "URBs in flight per endpoint (2..32, default 4); raised so that at least 64 frames stay queued.");
module_param(fixed_rate, uint, 0644);
MODULE_PARM_DESC(fixed_rate, "Only allow this sample rate (0 = any supported rate).");
module_param(reset_mixer, bool, 0444);
MODULE_PARM_DESC(reset_mixer, "Route playback N to output N at 0 dB and silence input monitoring at probe (default true).");
module_param(mac_out_offset, int, 0644);
MODULE_PARM_DESC(mac_out_offset, "Mac mode: frames to add to the playback position the device is told (default 0); applies from the next stream start.");

/* -- sample rates and bandwidth classes ----------------------------- */

static const struct ffuc_mode ffuc_pc_modes[] = {
	{ .alt = 1, .neps = 1, .words = 18, .pkt_frames = 8 },	/* 1x */
	{ .alt = 2, .neps = 1, .words = 14, .pkt_frames = 16 },	/* 2x */
	{ .alt = 3, .neps = 2, .words = 6, .pkt_frames = 32 },	/* 4x */
};

/* Packet sizes: IN = 4 + max_frames * 4 * ch, OUT = max_frames * 4 * (ch + 1),
 * rounded up by the device; alt 3 uses two transactions per microframe.
 */
static const struct ffuc_mode ffuc_mac_modes[] = {
	{ .alt = 1, .neps = 1, .words = 18, .in_mps = 508, .out_mps = 532, .max_frames = 7 },
	{ .alt = 2, .neps = 1, .words = 14, .in_mps = 732, .out_mps = 780, .max_frames = 13 },
	{ .alt = 3, .neps = 1, .words = 12, .in_mps = 1208, .out_mps = 1304, .max_frames = 25 },
};

static const unsigned int ffuc_rates[] = {
	32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000,
};

static const struct snd_pcm_hw_constraint_list ffuc_rate_list = {
	.count = ARRAY_SIZE(ffuc_rates),
	.list = ffuc_rates,
};

static unsigned int ffuc_mode_index(unsigned int rate)
{
	if (rate <= 48000)
		return 0;
	if (rate <= 96000)
		return 1;
	return 2;
}

static bool ffuc_rate_supported(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ffuc_rates); i++)
		if (ffuc_rates[i] == rate)
			return true;
	return false;
}

static unsigned int ffuc_max_channels(struct ffuc *chip, unsigned int rate)
{
	const struct ffuc_mode *m = &chip->modes[ffuc_mode_index(rate)];

	return m->neps * m->words;
}

/* Frames one URB moves at @rate (the same time span at every rate). */
static unsigned int ffuc_urb_frames(struct ffuc *chip, unsigned int rate)
{
	if (chip->iso)
		return chip->iso_packets * DIV_ROUND_UP(rate, 8000);
	return chip->base_urb_frames << ffuc_mode_index(rate);
}

/* Status register rate index -> rate (the vendor's ufx_id_to_freq). */
unsigned int ffuc_rate_from_index(unsigned int idx)
{
	static const unsigned int map[12] = {
		32000, 44100, 48000, 48000, 64000, 88200,
		96000, 96000, 128000, 176400, 192000, 192000,
	};

	return idx < ARRAY_SIZE(map) ? map[idx] : 0;
}

/* -- vendor requests ------------------------------------------------- */

#define FFUC_CTL_TIMEOUT	1000	/* ms */

int ffuc_write(struct ffuc *chip, u8 req, u16 val, u16 idx)
{
	if (chip->disconnected)
		return -ENODEV;
	return usb_control_msg_send(chip->dev, 0, req,
				    USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				    val, idx, NULL, 0, FFUC_CTL_TIMEOUT,
				    GFP_KERNEL);
}

/* wIndex must stay 0: a non-zero index turns the read into a write. */
int ffuc_read(struct ffuc *chip, u8 req, u32 *val)
{
	__le32 buf;
	int ret;

	if (chip->disconnected)
		return -ENODEV;
	ret = usb_control_msg_recv(chip->dev, 0, req,
				   USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				   0, 0, &buf, sizeof(buf), FFUC_CTL_TIMEOUT,
				   GFP_KERNEL);
	if (ret < 0)
		return ret;
	*val = le32_to_cpu(buf);
	return 0;
}

/* Channel init and global enable, as sent by the vendor driver on plug. */
static int ffuc_init_hw(struct ffuc *chip)
{
	int i, ret;

	for (i = 0; i < FFUC_MAX_CHANNELS; i++) {
		ret = ffuc_write(chip, FFUC_REQ_CHAN_INIT, 0x0001, i);
		if (ret < 0)
			return ret;
		ret = ffuc_write(chip, FFUC_REQ_CHAN_INIT, 0x0001, i + 0x20);
		if (ret < 0)
			return ret;
	}
	ret = ffuc_write(chip, FFUC_REQ_SETTINGS, 0x0800, 0xb800);
	if (ret < 0)
		return ret;
	return ffuc_write(chip, FFUC_REQ_ENABLE, 0x0001, 0x0000);
}

/*
 * Program the rate generator (the vendor's hwSetSampleRate).  0x1b takes
 * the DDS period 0x5f5e10000000 / rate, normalised to the single-speed
 * rate, plus the rate family in its low bits; 0x10 then selects the same
 * family in the settings register.  The multiplier (1x/2x/4x) comes from
 * the alternate setting, which must already be selected.
 */
static int ffuc_set_rate(struct ffuc *chip, unsigned int rate)
{
	unsigned int family;
	u32 dds;
	int ret;

	switch (rate) {
	case 32000: case 64000: case 128000:
		family = 0;
		break;
	case 44100: case 88200: case 176400:
		family = 1;
		break;
	default:
		family = 2;
		break;
	}

	dds = (u32)div_u64(0x5f5e10000000ULL, rate);
	if (rate > 114000)
		dds <<= 2;
	else if (rate > 57000)
		dds <<= 1;

	ret = ffuc_write(chip, FFUC_REQ_DDS, dds >> 16, (dds & 0xff00) | family);
	if (ret < 0)
		return ret;
	return ffuc_write(chip, FFUC_REQ_SETTINGS, family << 4,
			  FFUC_RATE_FAMILY_MASK);
}

static int ffuc_hw_mute(struct ffuc *chip, bool mute)
{
	return ffuc_write(chip, mute ? FFUC_REQ_MUTE : FFUC_REQ_UNMUTE,
			  0xffff, 0xc000);
}

/* -- sample format conversion ---------------------------------------
 * The device speaks float32.  Applications that ask for S32_LE get an
 * integer-only conversion here (no FPU use in the kernel).
 */

static u32 ffuc_s32_to_f32(s32 v)
{
	u32 sign = 0, mag, mant;
	int e;

	if (!v)
		return 0;
	if (v < 0) {
		sign = 0x80000000;
		mag = -(u32)v;
	} else {
		mag = v;
	}
	e = fls(mag) - 1;			/* 0..31 */
	if (e > 23)
		mant = mag >> (e - 23);
	else
		mant = mag << (23 - e);
	/* value = mag / 2^31 = 1.mant * 2^(e - 31) */
	return sign | ((u32)(e - 31 + 127) << 23) | (mant & 0x7fffff);
}

static s32 ffuc_f32_to_s32(u32 f)
{
	u32 exp = (f >> 23) & 0xff;
	u32 mant = (f & 0x7fffff) | 0x800000;
	bool neg = f >> 31;
	int shift;
	u32 mag;

	if (exp == 0xff && (f & 0x7fffff))
		return 0;			/* NaN */
	/* |value| * 2^31 = mant * 2^(exp - 127 - 23 + 31) */
	shift = (int)exp - 119;
	if (shift >= 8)
		return neg ? S32_MIN : S32_MAX;	/* |value| >= 1.0, clip */
	if (shift >= 0)
		mag = mant << shift;
	else if (shift > -24)
		mag = mant >> -shift;
	else
		return 0;
	return neg ? -(s32)mag : (s32)mag;
}

/* -- streaming engine ------------------------------------------------ */

/*
 * Copy @frames device frames into the capture ring at frame @pos.  Sample
 * c of frame f is src[f * fstride + c * cstride] (PC mode: interleaved,
 * Mac mode: channel by channel); device channel c is ALSA channel coff + c.
 */
static void ffuc_capture_copy(struct snd_pcm_runtime *rt, snd_pcm_uframes_t pos,
			      const __le32 *src, unsigned int frames,
			      unsigned int nch, unsigned int coff,
			      unsigned int fstride, unsigned int cstride)
{
	bool is_float = rt->format == SNDRV_PCM_FORMAT_FLOAT_LE;
	unsigned int chans = rt->channels;
	unsigned int p = pos % rt->buffer_size;
	unsigned int f, c;

	for (f = 0; f < frames; f++, src += fstride) {
		u32 *dst = (u32 *)rt->dma_area + p * chans;

		for (c = 0; c < nch && coff + c < chans; c++) {
			u32 v = le32_to_cpu(src[c * cstride]);

			dst[coff + c] = is_float ? v : (u32)ffuc_f32_to_s32(v);
		}
		if (++p == rt->buffer_size)
			p = 0;
	}
}

/* Copy @frames frames from the playback ring at @pos to dst[f * fstride + c]. */
static void ffuc_playback_copy(struct snd_pcm_runtime *rt, snd_pcm_uframes_t pos,
			       __le32 *dst, unsigned int frames,
			       unsigned int nch, unsigned int coff,
			       unsigned int fstride)
{
	bool is_float = rt->format == SNDRV_PCM_FORMAT_FLOAT_LE;
	unsigned int chans = rt->channels;
	unsigned int p = pos % rt->buffer_size;
	unsigned int f, c;

	for (f = 0; f < frames; f++, dst += fstride) {
		const u32 *src = (const u32 *)rt->dma_area + p * chans;

		for (c = 0; c < nch; c++) {
			u32 v = 0;

			if (coff + c < chans)
				v = is_float ? src[coff + c] :
				    ffuc_s32_to_f32((s32)src[coff + c]);
			dst[c] = cpu_to_le32(v);
		}
		if (++p == rt->buffer_size)
			p = 0;
	}
}

/* Account for @frames moved on endpoint @idx.  Returns true when the
 * pointer crossed a period boundary.  Called with the substream lock held.
 */
static bool ffuc_stream_advance(struct ffuc *chip, struct ffuc_stream *s,
				struct snd_pcm_runtime *rt, unsigned int idx,
				unsigned int frames)
{
	snd_pcm_uframes_t hw;
	int i;

	s->pos[idx] += frames;
	hw = s->pos[0];
	for (i = 1; i < chip->mode->neps; i++)
		hw = min(hw, s->pos[i]);
	s->hw_ptr = hw;
	if (hw / rt->period_size == s->period)
		return false;
	s->period = hw / rt->period_size;
	return true;
}

/* PC mode: one interrupt URB of interleaved frames. */
static void ffuc_capture_urb(struct ffuc_ep *ep, struct urb *urb)
{
	struct ffuc *chip = ep->chip;
	struct ffuc_stream *s = &chip->streams[SNDRV_PCM_STREAM_CAPTURE];
	struct snd_pcm_substream *subs = s->substream;
	const unsigned int words = chip->mode->words;
	unsigned int frames = urb->actual_length / (words * FFUC_SAMPLE_BYTES);
	unsigned long flags;

	if (!frames)
		return;

	snd_pcm_stream_lock_irqsave(subs, flags);
	if (s->running) {
		ffuc_capture_copy(subs->runtime, s->pos[ep->index],
				  urb->transfer_buffer, frames, words,
				  ep->index * words, words, 1);
		if (ffuc_stream_advance(chip, s, subs->runtime, ep->index, frames))
			snd_pcm_period_elapsed_under_stream_lock(subs);
	}
	snd_pcm_stream_unlock_irqrestore(subs, flags);
}

/* PC mode: fill the URB that is about to be resubmitted with the next frames. */
static void ffuc_playback_urb(struct ffuc_ep *ep, struct urb *urb)
{
	struct ffuc *chip = ep->chip;
	struct ffuc_stream *s = &chip->streams[SNDRV_PCM_STREAM_PLAYBACK];
	struct snd_pcm_substream *subs = s->substream;
	const unsigned int words = chip->mode->words;
	const unsigned int frames = chip->urb_frames;
	unsigned long flags;

	snd_pcm_stream_lock_irqsave(subs, flags);
	if (s->running) {
		ffuc_playback_copy(subs->runtime, s->pos[ep->index],
				   urb->transfer_buffer, frames, words,
				   ep->index * words, words);
		if (ffuc_stream_advance(chip, s, subs->runtime, ep->index, frames))
			snd_pcm_period_elapsed_under_stream_lock(subs);
	} else {
		memset(urb->transfer_buffer, 0, urb->transfer_buffer_length);
	}
	snd_pcm_stream_unlock_irqrestore(subs, flags);
}

/*
 * Mac mode, implicit feedback.  Every IN packet's device position and frame
 * count is remembered by packet number; OUT packet k is sent with the frame
 * count of IN packet k - fb_delay and is placed at that packet's position
 * plus out_lead.  The URBs of both endpoints start in the same microframe
 * and stay contiguous, so IN packet k - fb_delay was received fb_delay
 * microframes before OUT packet k goes out: out_lead covers those, plus the
 * small margin the vendor driver uses.  The OUT positions are then
 * continuous whenever the IN ones are.
 *
 * A lost IN packet is replaced by a nominal one continuing from the last
 * position.  Called with fb_lock held.
 */
static void ffuc_fb_put(struct ffuc *chip, unsigned int pos, unsigned int frames)
{
	chip->fb[chip->in_pkts++ & (FFUC_FB_SIZE - 1)] = pos << 5 | frames;
	chip->in_end = (pos + frames) % FFUC_ISO_COUNTER_MOD;
}

static void ffuc_fb_put_nominal(struct ffuc *chip)
{
	chip->fb_acc += chip->rate;
	ffuc_fb_put(chip, chip->in_end, chip->fb_acc / 8000);
	chip->fb_acc %= 8000;
}

/* The IN packet OUT packet @k mirrors, or false while there is none yet. */
static bool ffuc_fb_get(struct ffuc *chip, u64 k, unsigned int *pos,
			unsigned int *frames)
{
	u64 src = k - chip->fb_delay;
	u32 e;

	if (k < chip->fb_delay || src >= chip->in_pkts ||
	    chip->in_pkts - src > FFUC_FB_SIZE)
		return false;
	e = chip->fb[src & (FFUC_FB_SIZE - 1)];
	*pos = e >> 5;
	*frames = min(e & 0x1f, chip->mode->max_frames);
	return true;
}

/* Mac mode: one isochronous IN URB; each packet is a 4-byte header followed
 * by the frames stored channel by channel.
 */
static void ffuc_iso_capture_urb(struct ffuc_ep *ep, struct urb *urb)
{
	struct ffuc *chip = ep->chip;
	struct ffuc_stream *s = &chip->streams[SNDRV_PCM_STREAM_CAPTURE];
	struct snd_pcm_substream *subs = s->substream;
	const struct ffuc_mode *m = chip->mode;
	const unsigned int nch = m->words;
	unsigned int frames[FFUC_MAX_ISO_PACKETS];
	unsigned long flags;
	bool elapsed = false;
	int i;

	spin_lock_irqsave(&chip->fb_lock, flags);
	for (i = 0; i < urb->number_of_packets; i++) {
		const struct usb_iso_packet_descriptor *d = &urb->iso_frame_desc[i];

		const u8 *pkt = (const u8 *)urb->transfer_buffer + d->offset;
		u32 hdr;

		/* A payload that is not whole frames cannot be parsed. */
		if (d->status || d->actual_length < FFUC_ISO_IN_HEADER ||
		    (d->actual_length - FFUC_ISO_IN_HEADER) %
		    (nch * FFUC_SAMPLE_BYTES)) {
			frames[i] = 0;
			ffuc_fb_put_nominal(chip);
			continue;
		}
		frames[i] = min((d->actual_length - FFUC_ISO_IN_HEADER) /
				(nch * FFUC_SAMPLE_BYTES), m->max_frames);
		hdr = le32_to_cpup((const __le32 *)pkt);
		if ((hdr & 0x1f) != frames[i])
			dev_dbg_ratelimited(&chip->dev->dev,
					    "IN packet: %u bytes, header %#010x\n",
					    d->actual_length, hdr);
		ffuc_fb_put(chip, (hdr >> 5) & (FFUC_ISO_COUNTER_MOD - 1),
			    frames[i]);
	}
	spin_unlock_irqrestore(&chip->fb_lock, flags);

	snd_pcm_stream_lock_irqsave(subs, flags);
	if (s->running) {
		for (i = 0; i < urb->number_of_packets; i++) {
			const u8 *pkt = (const u8 *)urb->transfer_buffer +
					urb->iso_frame_desc[i].offset;

			if (!frames[i])
				continue;
			ffuc_capture_copy(subs->runtime, s->pos[0],
					  (const __le32 *)(pkt + FFUC_ISO_IN_HEADER),
					  frames[i], nch, 0, 1, frames[i]);
			elapsed |= ffuc_stream_advance(chip, s, subs->runtime, 0,
						       frames[i]);
		}
		if (elapsed)
			snd_pcm_period_elapsed_under_stream_lock(subs);
	}
	snd_pcm_stream_unlock_irqrestore(subs, flags);
}

/* Mac mode: lay out and fill the OUT URB that is about to be (re)submitted.
 * Each frame is a header word plus the samples; the header word of a
 * packet's first frame carries the position to play it at, the others are
 * 0.  Until the mirrored IN packets exist, the packets are empty.
 */
static void ffuc_iso_playback_fill(struct ffuc_ep *ep, struct urb *urb)
{
	struct ffuc *chip = ep->chip;
	struct ffuc_stream *s = &chip->streams[SNDRV_PCM_STREAM_PLAYBACK];
	struct snd_pcm_substream *subs = s->substream;
	const struct ffuc_mode *m = chip->mode;
	const unsigned int nch = m->words, fwords = nch + 1;
	unsigned int frames[FFUC_MAX_ISO_PACKETS], pos[FFUC_MAX_ISO_PACKETS];
	unsigned int off = 0, f;
	unsigned long flags;
	bool elapsed = false;
	int i;

	spin_lock_irqsave(&chip->fb_lock, flags);
	for (i = 0; i < urb->number_of_packets; i++)
		if (!ffuc_fb_get(chip, chip->out_pkts++, &pos[i], &frames[i]))
			frames[i] = 0;
	spin_unlock_irqrestore(&chip->fb_lock, flags);

	snd_pcm_stream_lock_irqsave(subs, flags);
	for (i = 0; i < urb->number_of_packets; i++) {
		__le32 *dst = (__le32 *)((u8 *)urb->transfer_buffer + off);
		unsigned int len = frames[i] * fwords * FFUC_SAMPLE_BYTES;

		urb->iso_frame_desc[i].offset = off;
		urb->iso_frame_desc[i].length = len;
		if (s->running) {
			ffuc_playback_copy(subs->runtime, s->pos[0], dst + 1,
					   frames[i], nch, 0, fwords);
			elapsed |= ffuc_stream_advance(chip, s, subs->runtime, 0,
						       frames[i]);
		} else {
			memset(dst, 0, len);
		}
		for (f = 0; f < frames[i]; f++)
			dst[f * fwords] = 0;
		if (frames[i])
			dst[0] = cpu_to_le32((pos[i] + chip->out_lead) %
					     FFUC_ISO_COUNTER_MOD);
		off += len;
	}
	urb->transfer_buffer_length = off;
	if (elapsed)
		snd_pcm_period_elapsed_under_stream_lock(subs);
	snd_pcm_stream_unlock_irqrestore(subs, flags);
}

/*
 * Mac mode: an URB queue that ran dry is restarted later by the host
 * controller without any error, and the IN/OUT packet pairing is off from
 * then on.  Catch that from the start frames (compared modulo 2048
 * microframes, which divides the range of every HCD).  Returns false on a
 * gap.
 */
static bool ffuc_iso_contiguous(struct ffuc_ep *ep, struct urb *urb)
{
	int expected = ep->next_start;

	ep->next_start = (urb->start_frame + urb->number_of_packets) &
			 FFUC_ISO_FRAME_MASK;
	return expected < 0 ||
	       ((urb->start_frame - expected) & FFUC_ISO_FRAME_MASK) == 0;
}

static void ffuc_stop_running_streams(struct ffuc *chip)
{
	int dir;

	for (dir = 0; dir < 2; dir++) {
		struct snd_pcm_substream *subs = chip->streams[dir].substream;

		if (subs)
			snd_pcm_stop_xrun(subs);
	}
}

static void ffuc_urb_complete(struct urb *urb)
{
	struct ffuc_urb *u = urb->context;
	struct ffuc_ep *ep = u->ep;
	struct ffuc *chip = ep->chip;
	int ret;

	switch (urb->status) {
	case 0:
		atomic_set(&chip->urb_errors, 0);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return;			/* killed or gone */
	case -EXDEV:			/* isochronous: some packets failed */
		if (chip->iso)
			break;
		fallthrough;
	default:
		/* Transient bus error: skip this URB's data, keep streaming. */
		dev_dbg_ratelimited(&chip->dev->dev, "%s URB status %d\n",
				    ep->dir ? "IN" : "OUT", urb->status);
		if (atomic_inc_return(&chip->urb_errors) > FFUC_MAX_URB_ERRORS) {
			dev_err(&chip->dev->dev, "too many URB errors, stopping\n");
			goto broken;
		}
		/* Mac mode handles the failed packets below. */
		if (!chip->iso) {
			if (ep->dir == SNDRV_PCM_STREAM_PLAYBACK)
				memset(urb->transfer_buffer, 0,
				       urb->transfer_buffer_length);
			goto resubmit;
		}
	}

	if (!READ_ONCE(chip->engine_running))
		return;

	if (ep->dir == SNDRV_PCM_STREAM_CAPTURE) {
		if (chip->iso)
			ffuc_iso_capture_urb(ep, urb);
		else
			ffuc_capture_urb(ep, urb);
	} else {
		if (chip->iso)
			ffuc_iso_playback_fill(ep, urb);
		else
			ffuc_playback_urb(ep, urb);
	}

resubmit:
	if (!READ_ONCE(chip->engine_running))
		return;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret == 0) {
		if (!READ_ONCE(chip->iso_frames_ok) ||
		    ffuc_iso_contiguous(ep, urb))
			return;
		dev_dbg_ratelimited(&chip->dev->dev, "%s URB queue ran dry\n",
				    ep->dir ? "IN" : "OUT");
		goto broken;
	}
	/* -EPERM: usb_kill_urb() is stopping the engine right now.  -ENOENT,
	 * -ENODEV, -ESHUTDOWN: the endpoint was disabled because the driver is
	 * being unbound or the device is gone; disconnect() cleans up.
	 */
	if (ret == -EPERM || ret == -ENOENT || ret == -ENODEV ||
	    ret == -ESHUTDOWN || !READ_ONCE(chip->engine_running))
		return;
	dev_err_ratelimited(&chip->dev->dev, "URB resubmit failed: %d\n", ret);
broken:
	if (!READ_ONCE(chip->engine_broken)) {
		WRITE_ONCE(chip->engine_broken, true);
		queue_work(system_unbound_wq, &chip->recover_work);
	}
}

/*
 * Status ticker (interface 5).  Byte 3 bit 7 is set once the device has
 * latched its outputs to silence after an OUT underrun; only selecting the
 * alternate setting again clears it.
 */
static void ffuc_status_complete(struct urb *urb)
{
	struct ffuc *chip = urb->context;
	int ret;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return;
	default:
		goto resubmit;	/* the watchdog is best effort */
	}

	if (urb->actual_length >= FFUC_STATUS_LEN &&
	    (chip->status_buf[3] & FFUC_STATUS_OUT_LATCHED) &&
	    time_after(jiffies, READ_ONCE(chip->status_armed_at)) &&
	    READ_ONCE(chip->engine_running) && !READ_ONCE(chip->engine_broken)) {
		WRITE_ONCE(chip->engine_broken, true);
		queue_work(system_unbound_wq, &chip->recover_work);
		return;		/* the restart resubmits it */
	}

resubmit:
	if (!READ_ONCE(chip->engine_running))
		return;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0 && ret != -EPERM)
		dev_dbg_ratelimited(&chip->dev->dev,
				    "status URB resubmit failed: %d\n", ret);
}

static void ffuc_kill_urbs(struct ffuc *chip)
{
	int dir, e, i;

	for (dir = 0; dir < 2; dir++)
		for (e = 0; e < FFUC_MAX_EPS; e++)
			for (i = 0; i < chip->nurbs; i++)
				usb_kill_urb(chip->eps[dir][e].urbs[i].urb);
	usb_kill_urb(chip->status_urb);
}

/* Called with chip->mutex held. */
static void ffuc_engine_stop(struct ffuc *chip)
{
	if (!chip->engine_running)
		return;
	WRITE_ONCE(chip->engine_running, false);
	ffuc_kill_urbs(chip);
	if (!chip->disconnected)
		ffuc_hw_mute(chip, true);
	dev_dbg(&chip->dev->dev, "engine stopped\n");
}

/* PC mode: interrupt URBs of @bytes on one or two endpoint pairs. */
static void ffuc_int_setup_urbs(struct ffuc *chip, unsigned int bytes)
{
	const struct ffuc_mode *m = chip->mode;
	int dir, e, i;

	for (dir = 0; dir < 2; dir++) {
		for (e = 0; e < m->neps; e++) {
			struct ffuc_ep *ep = &chip->eps[dir][e];
			unsigned int pipe;

			/* EP 0x01/0x82 for the first channel half, 0x03/0x84
			 * for the second one (alt 3 only).
			 */
			if (dir == SNDRV_PCM_STREAM_PLAYBACK)
				pipe = usb_sndintpipe(chip->dev, 1 + 2 * e);
			else
				pipe = usb_rcvintpipe(chip->dev, 2 + 2 * e);

			for (i = 0; i < chip->nurbs; i++) {
				struct ffuc_urb *u = &ep->urbs[i];

				usb_fill_int_urb(u->urb, chip->dev, pipe, u->buf,
						 bytes, ffuc_urb_complete, u, 1);
				u->urb->transfer_dma = u->dma;
				u->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
				if (dir == SNDRV_PCM_STREAM_PLAYBACK)
					memset(u->buf, 0, bytes);
			}
		}
	}
}

/* Mac mode: one isochronous endpoint pair, chip->iso_packets per URB. */
static void ffuc_iso_setup_urbs(struct ffuc *chip)
{
	const struct ffuc_mode *m = chip->mode;
	int dir, i, p;

	chip->in_pkts = chip->out_pkts = 0;
	chip->iso_frames_ok = false;	/* until the start has been checked */
	chip->in_end = 0;
	chip->fb_acc = 0;
	/* An OUT URB is refilled when it completes, behind nurbs - 1 queued
	 * ones; by then the IN URB before the one it ran alongside has
	 * certainly completed.  The vendor driver places OUT data
	 * 2 * max_frames - 2 frames ahead of the IN position.
	 */
	chip->fb_delay = (chip->nurbs + 1) * chip->iso_packets;
	chip->out_lead = (DIV_ROUND_CLOSEST(chip->fb_delay * chip->rate, 8000) +
			  2 * m->max_frames - 2 + mac_out_offset) &
			 (FFUC_ISO_COUNTER_MOD - 1);

	for (dir = 0; dir < 2; dir++) {
		struct ffuc_ep *ep = &chip->eps[dir][0];

		ep->next_start = -1;
		for (i = 0; i < chip->nurbs; i++) {
			struct urb *urb = ep->urbs[i].urb;

			urb->dev = chip->dev;
			urb->pipe = dir == SNDRV_PCM_STREAM_PLAYBACK ?
				    usb_sndisocpipe(chip->dev, 1) :
				    usb_rcvisocpipe(chip->dev, 2);
			urb->interval = 1;
			urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
			urb->transfer_buffer = ep->urbs[i].buf;
			urb->transfer_dma = ep->urbs[i].dma;
			urb->number_of_packets = chip->iso_packets;
			urb->complete = ffuc_urb_complete;
			urb->context = &ep->urbs[i];
			if (dir == SNDRV_PCM_STREAM_PLAYBACK) {
				ffuc_iso_playback_fill(ep, urb);	/* silence */
				continue;
			}
			urb->transfer_buffer_length = chip->iso_packets * m->in_mps;
			for (p = 0; p < chip->iso_packets; p++) {
				urb->iso_frame_desc[p].offset = p * m->in_mps;
				urb->iso_frame_desc[p].length = m->in_mps;
			}
		}
	}
}

/*
 * Mac mode: the first IN and OUT URBs may have been scheduled a frame apart
 * (the host controller starts an ASAP stream on a frame boundary).  The
 * OUT data must then be placed that much further ahead.  This runs before
 * the first OUT packet with data is filled, two URB periods from now.
 */
static void ffuc_iso_align(struct ffuc *chip)
{
	struct ffuc_ep *ein = &chip->eps[SNDRV_PCM_STREAM_CAPTURE][0];
	struct ffuc_ep *eout = &chip->eps[SNDRV_PCM_STREAM_PLAYBACK][0];
	int in = ein->urbs[0].urb->start_frame;
	int out = eout->urbs[0].urb->start_frame;
	int diff = out - in;	/* microframes at high speed */
	bool ok = true;
	unsigned long flags;
	int i;

	/* Only trust start frames that came out contiguous for the URBs that
	 * were just queued back to back; some host controllers do not track
	 * them.
	 */
	for (i = 0; i < chip->nurbs; i++) {
		ok &= ffuc_iso_contiguous(ein, ein->urbs[i].urb);
		ok &= ffuc_iso_contiguous(eout, eout->urbs[i].urb);
	}
	if (!ok || abs(diff) > 64)
		diff = 0;
	spin_lock_irqsave(&chip->fb_lock, flags);
	chip->out_lead = (chip->out_lead + FFUC_ISO_COUNTER_MOD +
			  diff * (int)chip->rate / 8000) %
			 FFUC_ISO_COUNTER_MOD;
	spin_unlock_irqrestore(&chip->fb_lock, flags);
	WRITE_ONCE(chip->iso_frames_ok, ok);
	dev_dbg(&chip->dev->dev, "start frames IN %d OUT %d%s, OUT lead %u\n",
		in, out, ok ? "" : " (not contiguous, unchecked)", chip->out_lead);
}

/* Called with chip->mutex held; chip->rate/mode are already set. */
static int ffuc_engine_start(struct ffuc *chip)
{
	const struct ffuc_mode *m = chip->mode;
	unsigned int bytes;
	int e, i, ret;

	if (chip->engine_running)
		return 0;
	if (chip->disconnected)
		return -ENODEV;

	chip->urb_frames = ffuc_urb_frames(chip, chip->rate);
	if (chip->iso)
		bytes = chip->iso_packets * max(m->in_mps, m->out_mps);
	else
		bytes = chip->urb_frames * m->words * FFUC_SAMPLE_BYTES;
	if (WARN_ON(bytes > chip->urb_bytes_max))
		return -EINVAL;

	/* Selecting the alt also resets the endpoints' data toggles. */
	ret = usb_set_interface(chip->dev, FFUC_IFACE, m->alt);
	if (ret < 0)
		return ret;
	ret = ffuc_set_rate(chip, chip->rate);
	if (ret < 0)
		return ret;

	if (chip->iso)
		ffuc_iso_setup_urbs(chip);
	else
		ffuc_int_setup_urbs(chip, bytes);

	atomic_set(&chip->urb_errors, 0);
	WRITE_ONCE(chip->engine_broken, false);
	WRITE_ONCE(chip->engine_running, true);
	for (i = 0; i < chip->nurbs; i++) {
		for (e = 0; e < m->neps; e++) {
			ret = usb_submit_urb(chip->eps[SNDRV_PCM_STREAM_CAPTURE][e].urbs[i].urb,
					     GFP_KERNEL);
			if (ret < 0)
				goto error;
			ret = usb_submit_urb(chip->eps[SNDRV_PCM_STREAM_PLAYBACK][e].urbs[i].urb,
					     GFP_KERNEL);
			if (ret < 0)
				goto error;
		}
	}

	if (chip->iso)
		ffuc_iso_align(chip);

	if (chip->status_intf) {
		usb_fill_int_urb(chip->status_urb, chip->dev,
				 usb_rcvintpipe(chip->dev, FFUC_STATUS_EP),
				 chip->status_buf, FFUC_STATUS_LEN,
				 ffuc_status_complete, chip, 1);
		WRITE_ONCE(chip->status_armed_at,
			   jiffies + msecs_to_jiffies(FFUC_STATUS_GRACE_MS));
		ret = usb_submit_urb(chip->status_urb, GFP_KERNEL);
		if (ret < 0)
			dev_warn(&chip->dev->dev,
				 "cannot watch the status endpoint: %d\n", ret);
	}

	ret = ffuc_hw_mute(chip, false);
	if (ret < 0)
		goto error;

	dev_dbg(&chip->dev->dev, "engine started: %u Hz, alt %u, %u frames x %u URBs\n",
		chip->rate, m->alt, chip->urb_frames, chip->nurbs);

	return 0;

error:
	dev_err(&chip->dev->dev, "failed to start streaming: %d\n", ret);
	WRITE_ONCE(chip->engine_running, false);
	ffuc_kill_urbs(chip);
	return ret;
}

/*
 * Restart the engine after the device latched its outputs or the URB
 * stream broke.  Running substreams get an XRUN (data was lost) and the
 * engine comes back up immediately, so applications recover with a plain
 * prepare/start.
 */
static void ffuc_recover_work(struct work_struct *work)
{
	struct ffuc *chip = container_of(work, struct ffuc, recover_work);
	int ret;

	mutex_lock(&chip->mutex);
	if (chip->disconnected || !chip->engine_running ||
	    !READ_ONCE(chip->engine_broken))
		goto unlock;

	chip->recoveries++;
	dev_warn_ratelimited(&chip->dev->dev,
			     "stream interrupted (output underrun or USB error), restarting (%u)\n",
			     chip->recoveries);
	ffuc_stop_running_streams(chip);
	ffuc_engine_stop(chip);
	if (chip->prepared_mask) {
		ret = ffuc_engine_start(chip);
		if (ret < 0)
			dev_err(&chip->dev->dev, "restart failed: %d\n", ret);
	}
unlock:
	mutex_unlock(&chip->mutex);
}

/* -- PCM ------------------------------------------------------------- */

static const struct snd_pcm_hardware ffuc_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_FLOAT_LE | SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_KNOT,
	.rate_min = 32000,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = FFUC_MAX_CHANNELS,
	.buffer_bytes_max = 4 * 1024 * 1024,
	.period_bytes_min = 32,
	.period_bytes_max = 1024 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

/* Fewer channels at higher rates (USB bandwidth). */
static int ffuc_rule_channels(struct snd_pcm_hw_params *params,
			      struct snd_pcm_hw_rule *rule)
{
	struct ffuc *chip = rule->private;
	struct snd_interval *r = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval t = {
		.min = 1,
		.max = ffuc_max_channels(chip, r->min),
		.integer = 1,
	};

	return snd_interval_refine(hw_param_interval(params,
				   SNDRV_PCM_HW_PARAM_CHANNELS), &t);
}

/* Restrict the rate by channel count, the fixed_rate parameter, and the
 * rate already in use by the other direction (one clock for both).
 */
static int ffuc_rule_rate(struct snd_pcm_hw_params *params,
			  struct snd_pcm_hw_rule *rule)
{
	struct snd_pcm_substream *subs = rule->private;
	struct ffuc *chip = snd_pcm_substream_chip(subs);
	struct snd_interval *c = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	unsigned int other = 1 << !subs->stream;
	struct snd_interval t = { .min = 32000, .max = 192000, .integer = 1 };
	unsigned int locked = 0;

	if (c->min > 14)
		t.max = 48000;
	else if (c->min > 12)
		t.max = 96000;

	if (READ_ONCE(chip->hw_params_mask) & other)
		locked = READ_ONCE(chip->rate);
	else if (ffuc_rate_supported(READ_ONCE(fixed_rate)))
		locked = READ_ONCE(fixed_rate);
	if (locked) {
		t.min = max(t.min, locked);
		t.max = min(t.max, locked);
	}
	return snd_interval_refine(hw_param_interval(params,
				   SNDRV_PCM_HW_PARAM_RATE), &t);
}

/* A period must span at least one URB. */
static int ffuc_rule_period(struct snd_pcm_hw_params *params,
			    struct snd_pcm_hw_rule *rule)
{
	struct ffuc *chip = rule->private;
	struct snd_interval *r = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval t = {
		.min = ffuc_urb_frames(chip, r->min),
		.max = UINT_MAX,
		.integer = 1,
	};

	return snd_interval_refine(hw_param_interval(params,
				   SNDRV_PCM_HW_PARAM_PERIOD_SIZE), &t);
}

static int ffuc_pcm_open(struct snd_pcm_substream *subs)
{
	struct ffuc *chip = snd_pcm_substream_chip(subs);
	struct snd_pcm_runtime *rt = subs->runtime;
	int ret;

	if (chip->disconnected)
		return -ENODEV;

	rt->hw = ffuc_pcm_hw;
	ret = snd_pcm_hw_constraint_list(rt, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &ffuc_rate_list);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_rule_add(rt, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
				  ffuc_rule_channels, chip,
				  SNDRV_PCM_HW_PARAM_RATE, -1);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_rule_add(rt, 0, SNDRV_PCM_HW_PARAM_RATE,
				  ffuc_rule_rate, subs,
				  SNDRV_PCM_HW_PARAM_CHANNELS, -1);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_rule_add(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  ffuc_rule_period, chip,
				  SNDRV_PCM_HW_PARAM_RATE, -1);
	if (ret < 0)
		return ret;
	/* The device is float32 internally; S32 is converted exactly for the
	 * 24 bits the converters deliver.
	 */
	return snd_pcm_hw_constraint_msbits(rt, 0, 32, 24);
}

static int ffuc_pcm_close(struct snd_pcm_substream *subs)
{
	return 0;
}

static int ffuc_pcm_hw_params(struct snd_pcm_substream *subs,
			      struct snd_pcm_hw_params *params)
{
	struct ffuc *chip = snd_pcm_substream_chip(subs);
	unsigned int rate = params_rate(params);
	unsigned int self = 1 << subs->stream;
	int ret = 0;

	if (params_channels(params) > ffuc_max_channels(chip, rate))
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (rate != chip->rate || !chip->mode) {
		if ((chip->hw_params_mask | chip->prepared_mask) & ~self) {
			/* The other direction runs at another rate. */
			ret = -EBUSY;
			goto unlock;
		}
		ffuc_engine_stop(chip);
		chip->prepared_mask &= ~self;
		chip->rate = rate;
		chip->mode = &chip->modes[ffuc_mode_index(rate)];
	}
	chip->hw_params_mask |= self;
unlock:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int ffuc_pcm_hw_free(struct snd_pcm_substream *subs)
{
	struct ffuc *chip = snd_pcm_substream_chip(subs);
	unsigned int self = 1 << subs->stream;

	mutex_lock(&chip->mutex);
	chip->hw_params_mask &= ~self;
	chip->prepared_mask &= ~self;
	if (!chip->prepared_mask)
		ffuc_engine_stop(chip);
	mutex_unlock(&chip->mutex);
	return 0;
}

static int ffuc_pcm_prepare(struct snd_pcm_substream *subs)
{
	struct ffuc *chip = snd_pcm_substream_chip(subs);
	struct ffuc_stream *s = &chip->streams[subs->stream];
	int ret;

	mutex_lock(&chip->mutex);
	if (READ_ONCE(chip->engine_broken))
		ffuc_engine_stop(chip);
	ret = ffuc_engine_start(chip);
	if (ret == 0)
		chip->prepared_mask |= 1 << subs->stream;
	mutex_unlock(&chip->mutex);
	if (ret < 0)
		return ret;

	snd_pcm_stream_lock_irq(subs);
	s->running = false;
	memset(s->pos, 0, sizeof(s->pos));
	s->hw_ptr = 0;
	s->period = 0;
	/* Frames queued in OUT URBs between the ring buffer and the DACs. */
	if (subs->stream == SNDRV_PCM_STREAM_PLAYBACK)
		subs->runtime->delay = chip->nurbs * chip->urb_frames;
	snd_pcm_stream_unlock_irq(subs);
	return 0;
}

/* Called with the substream lock held. */
static int ffuc_pcm_trigger(struct snd_pcm_substream *subs, int cmd)
{
	struct ffuc *chip = snd_pcm_substream_chip(subs);
	struct ffuc_stream *s = &chip->streams[subs->stream];

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!READ_ONCE(chip->engine_running))
			return -EIO;
		memset(s->pos, 0, sizeof(s->pos));
		s->hw_ptr = 0;
		s->period = 0;
		s->running = true;
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		s->running = false;
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t ffuc_pcm_pointer(struct snd_pcm_substream *subs)
{
	struct ffuc *chip = snd_pcm_substream_chip(subs);

	return chip->streams[subs->stream].hw_ptr % subs->runtime->buffer_size;
}

static const struct snd_pcm_ops ffuc_pcm_ops = {
	.open = ffuc_pcm_open,
	.close = ffuc_pcm_close,
	.hw_params = ffuc_pcm_hw_params,
	.hw_free = ffuc_pcm_hw_free,
	.prepare = ffuc_pcm_prepare,
	.trigger = ffuc_pcm_trigger,
	.pointer = ffuc_pcm_pointer,
};

static int ffuc_create_pcm(struct ffuc *chip)
{
	struct snd_pcm *pcm;
	int ret;

	ret = snd_pcm_new(chip->card, "Fireface UC", 0, 1, 1, &pcm);
	if (ret < 0)
		return ret;
	pcm->private_data = chip;
	strscpy(pcm->name, "Fireface UC", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ffuc_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &ffuc_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	chip->streams[SNDRV_PCM_STREAM_PLAYBACK].substream =
		pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	chip->streams[SNDRV_PCM_STREAM_CAPTURE].substream =
		pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
	chip->pcm = pcm;
	return 0;
}

/* -- USB driver ------------------------------------------------------ */

static int ffuc_alloc_urbs(struct ffuc *chip)
{
	int dir, e, i;

	chip->status_urb = usb_alloc_urb(0, GFP_KERNEL);
	chip->status_buf = kmalloc(FFUC_STATUS_LEN, GFP_KERNEL);
	if (!chip->status_urb || !chip->status_buf)
		return -ENOMEM;

	for (dir = 0; dir < 2; dir++) {
		for (e = 0; e < FFUC_MAX_EPS; e++) {
			struct ffuc_ep *ep = &chip->eps[dir][e];

			ep->chip = chip;
			ep->dir = dir;
			ep->index = e;
			for (i = 0; i < chip->nurbs; i++) {
				struct ffuc_urb *u = &ep->urbs[i];

				u->ep = ep;
				u->urb = usb_alloc_urb(chip->iso ? FFUC_MAX_ISO_PACKETS : 0,
						       GFP_KERNEL);
				if (!u->urb)
					return -ENOMEM;
				u->buf = usb_alloc_coherent(chip->dev,
							    chip->urb_bytes_max,
							    GFP_KERNEL, &u->dma);
				if (!u->buf)
					return -ENOMEM;
			}
		}
	}
	return 0;
}

static void ffuc_free_urbs(struct ffuc *chip)
{
	int dir, e, i;

	if (chip->status_urb) {
		usb_kill_urb(chip->status_urb);
		usb_free_urb(chip->status_urb);
		chip->status_urb = NULL;
	}
	kfree(chip->status_buf);
	chip->status_buf = NULL;

	for (dir = 0; dir < 2; dir++) {
		for (e = 0; e < FFUC_MAX_EPS; e++) {
			for (i = 0; i < FFUC_MAX_URBS; i++) {
				struct ffuc_urb *u = &chip->eps[dir][e].urbs[i];

				if (u->urb) {
					usb_kill_urb(u->urb);
					usb_free_urb(u->urb);
					u->urb = NULL;
				}
				if (u->buf) {
					usb_free_coherent(chip->dev,
							  chip->urb_bytes_max,
							  u->buf, u->dma);
					u->buf = NULL;
				}
			}
		}
	}
}

static void ffuc_card_free(struct snd_card *card)
{
	struct ffuc *chip = card->private_data;

	cancel_work_sync(&chip->recover_work);
	ffuc_free_urbs(chip);
	usb_put_dev(chip->dev);
	mutex_lock(&devices_mutex);
	clear_bit(chip->dev_idx, &devices_used);
	mutex_unlock(&devices_mutex);
}

/* Bring the device to a known state.  Called at probe and resume. */
static int ffuc_hw_setup(struct ffuc *chip)
{
	int ret;

	ret = usb_set_interface(chip->dev, FFUC_IFACE, chip->mode->alt);
	if (ret < 0)
		return ret;
	ret = ffuc_init_hw(chip);
	if (ret < 0)
		return ret;
	ret = ffuc_set_rate(chip, chip->rate);
	if (ret < 0)
		return ret;
	ret = ffuc_settings_apply(chip);
	if (ret < 0)
		return ret;
	return ffuc_hw_mute(chip, true);
}

static int ffuc_probe(struct usb_interface *intf,
		      const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct snd_card *card;
	struct ffuc *chip;
	u32 rev = 0;
	int dev_idx, ret;

	if (intf->cur_altsetting->desc.bInterfaceNumber != FFUC_IFACE)
		return -ENODEV;

	mutex_lock(&devices_mutex);
	for (dev_idx = 0; dev_idx < SNDRV_CARDS; dev_idx++)
		if (enable[dev_idx] && !test_bit(dev_idx, &devices_used))
			break;
	if (dev_idx >= SNDRV_CARDS) {
		mutex_unlock(&devices_mutex);
		return -ENODEV;
	}
	set_bit(dev_idx, &devices_used);
	mutex_unlock(&devices_mutex);

	ret = snd_card_new(&intf->dev, index[dev_idx], id[dev_idx], THIS_MODULE,
			   sizeof(*chip), &card);
	if (ret < 0)
		goto release_idx;

	chip = card->private_data;
	chip->card = card;
	chip->dev = usb_get_dev(dev);
	chip->intf = intf;
	chip->dev_idx = dev_idx;
	chip->iso = usb_id->driver_info == FFUC_TRANSPORT_ISO;
	chip->modes = chip->iso ? ffuc_mac_modes : ffuc_pc_modes;
	mutex_init(&chip->mutex);
	spin_lock_init(&chip->fb_lock);
	INIT_WORK(&chip->recover_work, ffuc_recover_work);
	chip->nurbs = clamp(nurbs, 2u, (unsigned int)FFUC_MAX_URBS);
	chip->base_urb_frames = clamp(round_up(urb_frames, 8), 8u, 256u);
	if (chip->nurbs * chip->base_urb_frames < FFUC_MIN_QUEUE_FRAMES) {
		chip->nurbs = DIV_ROUND_UP(FFUC_MIN_QUEUE_FRAMES, chip->base_urb_frames);
		dev_info(&intf->dev, "using %u URBs to keep %u frames queued\n",
			 chip->nurbs, FFUC_MIN_QUEUE_FRAMES);
	}
	/* About as many frames per URB in Mac mode as in PC mode. */
	chip->iso_packets = clamp(DIV_ROUND_UP(chip->base_urb_frames, 6), 1u,
				  (unsigned int)FFUC_MAX_ISO_PACKETS);
	/* Largest URB of all modes.  PC: 18 ch at 1x, 14 ch x2 at 2x, 6 ch x4
	 * at 4x.  Mac: the 4x OUT packet.
	 */
	if (chip->iso)
		chip->urb_bytes_max = chip->iso_packets *
				      ffuc_mac_modes[2].out_mps;
	else
		chip->urb_bytes_max = chip->base_urb_frames * 28 *
				      FFUC_SAMPLE_BYTES;
	chip->rate = 48000;
	chip->mode = &chip->modes[0];
	card->private_free = ffuc_card_free;
	dev_idx = -1;		/* released by ffuc_card_free() from now on */

	strscpy(card->driver, "FirefaceUC", sizeof(card->driver));
	strscpy(card->shortname, "Fireface UC", sizeof(card->shortname));
	strscpy(card->mixername, "Fireface UC", sizeof(card->mixername));
	if (!id[chip->dev_idx])
		snd_card_set_id(card, "FirefaceUC");

	ret = ffuc_alloc_urbs(chip);
	if (ret < 0)
		goto free_card;

	/* The status ticker is the only way to notice latched outputs (PC
	 * mode; Mac mode does not latch and has no such interface).
	 */
	chip->status_intf = chip->iso ? NULL :
			    usb_ifnum_to_if(dev, FFUC_STATUS_IFACE);
	if (chip->status_intf &&
	    usb_driver_claim_interface(&ffuc_driver, chip->status_intf, NULL) < 0) {
		dev_warn(&intf->dev, "interface %d busy, no underrun recovery\n",
			 FFUC_STATUS_IFACE);
		chip->status_intf = NULL;
	}

	ret = ffuc_read(chip, FFUC_REQ_REVISION, &rev);
	if (ret < 0) {
		dev_err(&intf->dev, "cannot read firmware revision: %d\n", ret);
		goto free_card;
	}
	snprintf(card->longname, sizeof(card->longname),
		 "RME Fireface UC (%s mode, firmware %u) at %s",
		 chip->iso ? "Mac" : "PC", (rev >> 16) & 0xff,
		 dev_name(&dev->dev));

	ret = ffuc_settings_load(chip);
	if (ret < 0)
		goto free_card;
	ret = ffuc_hw_setup(chip);
	if (ret < 0) {
		dev_err(&intf->dev, "hardware init failed: %d\n", ret);
		goto free_card;
	}
	if (reset_mixer) {
		ret = ffuc_mixer_reset(chip);
		if (ret < 0) {
			dev_err(&intf->dev, "mixer init failed: %d\n", ret);
			goto free_card;
		}
	}

	ret = ffuc_create_pcm(chip);
	if (ret < 0)
		goto free_card;
	ret = ffuc_create_controls(chip);
	if (ret < 0)
		goto free_card;
	ret = snd_card_register(card);
	if (ret < 0)
		goto free_card;

	usb_set_intfdata(intf, chip);
	dev_info(&intf->dev, "%s, card %d, %u frames x %u URBs\n",
		 card->longname, card->number, chip->base_urb_frames,
		 chip->nurbs);
	return 0;

free_card:
	if (chip->status_intf)
		usb_driver_release_interface(&ffuc_driver, chip->status_intf);
	snd_card_free(card);
release_idx:
	if (dev_idx >= 0) {
		mutex_lock(&devices_mutex);
		clear_bit(dev_idx, &devices_used);
		mutex_unlock(&devices_mutex);
	}
	return ret;
}

static void ffuc_disconnect(struct usb_interface *intf)
{
	struct ffuc *chip = usb_get_intfdata(intf);

	if (!chip)
		return;
	usb_set_intfdata(intf, NULL);

	snd_card_disconnect(chip->card);
	mutex_lock(&chip->mutex);
	chip->disconnected = true;
	WRITE_ONCE(chip->engine_running, false);
	ffuc_kill_urbs(chip);
	mutex_unlock(&chip->mutex);
	cancel_work_sync(&chip->recover_work);
	/* No-op if interface 5 was already unbound by the same unplug. */
	if (chip->status_intf)
		usb_driver_release_interface(&ffuc_driver, chip->status_intf);
	snd_card_free_when_closed(chip->card);
}

static int ffuc_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct ffuc *chip = usb_get_intfdata(intf);

	if (!chip)
		return 0;
	snd_power_change_state(chip->card, SNDRV_CTL_POWER_D3hot);
	snd_pcm_suspend_all(chip->pcm);
	cancel_work_sync(&chip->recover_work);
	mutex_lock(&chip->mutex);
	ffuc_engine_stop(chip);
	chip->prepared_mask = 0;
	mutex_unlock(&chip->mutex);
	return 0;
}

static int ffuc_resume(struct usb_interface *intf)
{
	struct ffuc *chip = usb_get_intfdata(intf);
	int ret;

	if (!chip)
		return 0;
	mutex_lock(&chip->mutex);
	ret = ffuc_hw_setup(chip);
	if (ret == 0 && reset_mixer)
		ret = ffuc_mixer_reset(chip);
	mutex_unlock(&chip->mutex);
	snd_power_change_state(chip->card, SNDRV_CTL_POWER_D0);
	return ret;
}

static const struct usb_device_id ffuc_ids[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(FFUC_VENDOR_ID, FFUC_PRODUCT_PC, FFUC_IFACE),
	  .driver_info = FFUC_TRANSPORT_INT },
	/* Mac mode reports RME's own vendor ID (older firmware: the SMSC one) */
	{ USB_DEVICE_INTERFACE_NUMBER(FFUC_VENDOR_RME, FFUC_PRODUCT_MAC, FFUC_IFACE),
	  .driver_info = FFUC_TRANSPORT_ISO },
	{ USB_DEVICE_INTERFACE_NUMBER(FFUC_VENDOR_ID, FFUC_PRODUCT_MAC, FFUC_IFACE),
	  .driver_info = FFUC_TRANSPORT_ISO },
	{ }
};
MODULE_DEVICE_TABLE(usb, ffuc_ids);

static struct usb_driver ffuc_driver = {
	.name = "snd-fireface-uc",
	.probe = ffuc_probe,
	.disconnect = ffuc_disconnect,
	.suspend = ffuc_suspend,
	.resume = ffuc_resume,
	.reset_resume = ffuc_resume,
	.id_table = ffuc_ids,
	.supports_autosuspend = 0,
};

module_usb_driver(ffuc_driver);

MODULE_DESCRIPTION("RME Fireface UC USB audio driver (PC and Mac mode)");
MODULE_LICENSE("GPL");
