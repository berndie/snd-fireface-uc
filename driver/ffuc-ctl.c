// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Fireface UC - ALSA controls
 *
 * The device has two settings registers that are written with masked
 * writes and can be read back:
 *
 *   FFUC_REQ_LEVELS   (0x17) analog reference levels (and 48V/PAD/Hi-Z,
 *                            which this driver never touches)
 *   FFUC_REQ_SETTINGS (0x10) clock source and digital I/O options
 *
 * The driver keeps a mirror of the bits it owns, seeded from the device
 * at probe, and rewrites them whenever a control changes.
 */
#include <linux/kernel.h>
#include <sound/control.h>
#include <sound/core.h>

#include "ffuc.h"

enum ffuc_reg {
	FFUC_REG_LEVELS,
	FFUC_REG_SETTINGS,
};

struct ffuc_enum {
	const char *name;
	enum ffuc_reg reg;
	unsigned int shift;
	unsigned int mask;		/* field mask before shifting */
	const char * const *texts;
	const u8 *raw;			/* register value of each item */
	unsigned int count;
	unsigned int def;		/* item used if the device holds junk */
};

static const char * const ffuc_out_texts[] = { "Hi Gain", "+4 dBu", "-10 dBV" };
static const char * const ffuc_in_texts[] = { "Lo Gain", "+4 dBu", "-10 dBV" };
static const char * const ffuc_clock_texts[] = {
	"Internal", "SPDIF Coaxial", "Optical", "Word Clock",
};
static const char * const ffuc_optical_texts[] = { "ADAT", "SPDIF" };
static const char * const ffuc_spdif_texts[] = { "Consumer", "Professional" };

static const u8 ffuc_line_out_raw[] = { 2, 3, 1 };
static const u8 ffuc_line_in_raw[] = { 0, 1, 3 };
static const u8 ffuc_phones_raw[] = { 1, 3, 2 };
static const u8 ffuc_clock_raw[] = { 1, 2, 4, 6 };
static const u8 ffuc_bit_raw[] = { 0, 1 };

static const struct ffuc_enum ffuc_enums[] = {
	{
		.name = "Line Out Level",
		.reg = FFUC_REG_LEVELS, .shift = FFUC_LINE_OUT_SHIFT, .mask = 3,
		.texts = ffuc_out_texts, .raw = ffuc_line_out_raw,
		.count = ARRAY_SIZE(ffuc_out_texts), .def = 1,
	}, {
		.name = "Phones Level",
		.reg = FFUC_REG_LEVELS, .shift = FFUC_PHONES_SHIFT, .mask = 3,
		.texts = ffuc_out_texts, .raw = ffuc_phones_raw,
		.count = ARRAY_SIZE(ffuc_out_texts), .def = 2,
	}, {
		.name = "Line In Level",
		.reg = FFUC_REG_LEVELS, .shift = FFUC_LINE_IN_SHIFT, .mask = 3,
		.texts = ffuc_in_texts, .raw = ffuc_line_in_raw,
		.count = ARRAY_SIZE(ffuc_in_texts), .def = 1,
	}, {
		.name = "Clock Source",
		.reg = FFUC_REG_SETTINGS, .shift = 0, .mask = FFUC_CLOCK_MASK,
		.texts = ffuc_clock_texts, .raw = ffuc_clock_raw,
		.count = ARRAY_SIZE(ffuc_clock_texts), .def = 0,
	}, {
		.name = "Optical Output Format",
		.reg = FFUC_REG_SETTINGS, .shift = 10, .mask = 1,
		.texts = ffuc_optical_texts, .raw = ffuc_bit_raw,
		.count = ARRAY_SIZE(ffuc_optical_texts), .def = 0,
	}, {
		.name = "SPDIF Output Format",
		.reg = FFUC_REG_SETTINGS, .shift = 7, .mask = 1,
		.texts = ffuc_spdif_texts, .raw = ffuc_bit_raw,
		.count = ARRAY_SIZE(ffuc_spdif_texts), .def = 0,
	},
};

static u16 *ffuc_reg_ptr(struct ffuc *chip, enum ffuc_reg reg)
{
	return reg == FFUC_REG_LEVELS ? &chip->levels : &chip->settings;
}

static int ffuc_enum_item(const struct ffuc_enum *e, u16 regval)
{
	unsigned int raw = (regval >> e->shift) & e->mask;
	int i;

	for (i = 0; i < e->count; i++)
		if (e->raw[i] == raw)
			return i;
	return -1;
}

static u16 ffuc_enum_set(const struct ffuc_enum *e, u16 regval, unsigned int item)
{
	regval &= ~(e->mask << e->shift);
	return regval | (e->raw[item] << e->shift);
}

static int ffuc_write_reg(struct ffuc *chip, enum ffuc_reg reg, u16 val)
{
	if (reg == FFUC_REG_LEVELS)
		return ffuc_write(chip, FFUC_REQ_LEVELS, val & FFUC_LEVELS_MASK,
				  FFUC_LEVELS_MASK);
	return ffuc_write(chip, FFUC_REQ_SETTINGS,
			  FFUC_SETTINGS_FIXED | (val & FFUC_SETTINGS_MASK),
			  FFUC_SETTINGS_MASK);
}

/* Seed the mirrors from the device; replace fields holding junk. */
int ffuc_settings_load(struct ffuc *chip)
{
	u32 levels, settings;
	int i, ret;

	ret = ffuc_read(chip, FFUC_REQ_LEVELS, &levels);
	if (ret < 0)
		return ret;
	ret = ffuc_read(chip, FFUC_REQ_SETTINGS, &settings);
	if (ret < 0)
		return ret;

	chip->levels = levels & FFUC_LEVELS_MASK;
	chip->settings = settings & FFUC_SETTINGS_MASK & ~FFUC_SETTINGS_FIXED;
	for (i = 0; i < ARRAY_SIZE(ffuc_enums); i++) {
		const struct ffuc_enum *e = &ffuc_enums[i];
		u16 *reg = ffuc_reg_ptr(chip, e->reg);

		if (ffuc_enum_item(e, *reg) < 0)
			*reg = ffuc_enum_set(e, *reg, e->def);
	}
	dev_dbg(&chip->dev->dev, "levels %#06x -> %#06x, settings %#06x -> %#06x\n",
		levels, chip->levels, settings, chip->settings);
	return 0;
}

int ffuc_settings_apply(struct ffuc *chip)
{
	int ret;

	ret = ffuc_write_reg(chip, FFUC_REG_LEVELS, chip->levels);
	if (ret < 0)
		return ret;
	return ffuc_write_reg(chip, FFUC_REG_SETTINGS, chip->settings);
}

/*
 * Default DSP routing: playback N -> output N at 0 dB, no input
 * monitoring, all output masters at 0 dB, loopback off.
 */
int ffuc_mixer_reset(struct ffuc *chip)
{
	int out, src, ret;

	for (out = 0; out < FFUC_MIX_OUTPUTS; out++) {
		for (src = 0; src < FFUC_MIX_STRIDE; src++) {
			u16 val = src == FFUC_MIX_PLAYBACK + out ? FFUC_MIX_UNITY : 0;

			ret = ffuc_write(chip, FFUC_REQ_MIXER, val,
					 out * FFUC_MIX_STRIDE + src);
			if (ret < 0)
				return ret;
		}
		ret = ffuc_write(chip, FFUC_REQ_MIXER, FFUC_MIX_UNITY,
				 FFUC_MIX_MASTER + out);
		if (ret < 0)
			return ret;
		ret = ffuc_write(chip, FFUC_REQ_LOOPBACK, 0, out);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* -- enumerated settings --------------------------------------------- */

static int ffuc_enum_info(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_info *info)
{
	const struct ffuc_enum *e = &ffuc_enums[kctl->private_value];

	return snd_ctl_enum_info(info, 1, e->count, e->texts);
}

static int ffuc_enum_get(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *val)
{
	struct ffuc *chip = snd_kcontrol_chip(kctl);
	const struct ffuc_enum *e = &ffuc_enums[kctl->private_value];
	int item;

	mutex_lock(&chip->mutex);
	item = ffuc_enum_item(e, *ffuc_reg_ptr(chip, e->reg));
	mutex_unlock(&chip->mutex);
	val->value.enumerated.item[0] = max(item, 0);
	return 0;
}

static int ffuc_enum_put(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *val)
{
	struct ffuc *chip = snd_kcontrol_chip(kctl);
	const struct ffuc_enum *e = &ffuc_enums[kctl->private_value];
	unsigned int item = val->value.enumerated.item[0];
	u16 *reg, newval;
	int ret;

	if (item >= e->count)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	reg = ffuc_reg_ptr(chip, e->reg);
	newval = ffuc_enum_set(e, *reg, item);
	if (newval == *reg) {
		ret = 0;
		goto unlock;
	}
	ret = ffuc_write_reg(chip, e->reg, newval);
	if (ret < 0)
		goto unlock;
	*reg = newval;
	ret = 1;
unlock:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* -- word clock single speed ----------------------------------------- */

static int ffuc_single_speed_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *val)
{
	struct ffuc *chip = snd_kcontrol_chip(kctl);

	mutex_lock(&chip->mutex);
	val->value.integer.value[0] = !!(chip->settings & FFUC_SINGLE_SPEED);
	mutex_unlock(&chip->mutex);
	return 0;
}

static int ffuc_single_speed_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *val)
{
	struct ffuc *chip = snd_kcontrol_chip(kctl);
	u16 newval;
	int ret;

	mutex_lock(&chip->mutex);
	newval = chip->settings & ~FFUC_SINGLE_SPEED;
	if (val->value.integer.value[0])
		newval |= FFUC_SINGLE_SPEED;
	if (newval == chip->settings) {
		ret = 0;
		goto unlock;
	}
	ret = ffuc_write_reg(chip, FFUC_REG_SETTINGS, newval);
	if (ret < 0)
		goto unlock;
	chip->settings = newval;
	ret = 1;
unlock:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* -- current sample rate (read-only, from the status register) ------ */

static int ffuc_rate_info(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 192000;
	return 0;
}

static int ffuc_rate_get(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *val)
{
	struct ffuc *chip = snd_kcontrol_chip(kctl);
	u32 status;
	int ret;

	mutex_lock(&chip->mutex);
	ret = ffuc_read(chip, FFUC_REQ_STATUS, &status);
	mutex_unlock(&chip->mutex);
	if (ret < 0)
		return ret;
	val->value.integer.value[0] = ffuc_rate_from_index(status >> 24);
	return 0;
}

static const struct snd_kcontrol_new ffuc_misc_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Word Clock Single Speed Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = ffuc_single_speed_get,
		.put = ffuc_single_speed_put,
	}, {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Sample Rate",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = ffuc_rate_info,
		.get = ffuc_rate_get,
	},
};

int ffuc_create_controls(struct ffuc *chip)
{
	struct snd_kcontrol_new tmpl = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.info = ffuc_enum_info,
		.get = ffuc_enum_get,
		.put = ffuc_enum_put,
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(ffuc_enums); i++) {
		tmpl.name = ffuc_enums[i].name;
		tmpl.private_value = i;
		ret = snd_ctl_add(chip->card, snd_ctl_new1(&tmpl, chip));
		if (ret < 0)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(ffuc_misc_controls); i++) {
		ret = snd_ctl_add(chip->card,
				  snd_ctl_new1(&ffuc_misc_controls[i], chip));
		if (ret < 0)
			return ret;
	}
	return 0;
}
