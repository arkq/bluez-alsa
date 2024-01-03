/*
 * BlueALSA - a2dp.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include <glib.h>

#if ENABLE_AAC
# include "a2dp-aac.h"
#endif
#if ENABLE_APTX
# include "a2dp-aptx.h"
#endif
#if ENABLE_APTX_HD
# include "a2dp-aptx-hd.h"
#endif
#if ENABLE_FASTSTREAM
# include "a2dp-faststream.h"
#endif
#if ENABLE_LC3PLUS
# include "a2dp-lc3plus.h"
#endif
#if ENABLE_LDAC
# include "a2dp-ldac.h"
#endif
#if ENABLE_MPEG
# include "a2dp-mpeg.h"
#endif
#include "a2dp-sbc.h"
#include "ba-transport.h"
#include "bluealsa-config.h"
#include "codec-sbc.h"
#include "shared/a2dp-codecs.h"
#include "shared/bluetooth.h"
#include "shared/log.h"

struct a2dp_codec * const a2dp_codecs[] = {
#if ENABLE_LC3PLUS
	&a2dp_lc3plus_source,
	&a2dp_lc3plus_sink,
#endif
#if ENABLE_LDAC
	&a2dp_ldac_source,
# if HAVE_LDAC_DECODE
	&a2dp_ldac_sink,
# endif
#endif
#if ENABLE_APTX_HD
	&a2dp_aptx_hd_source,
# if HAVE_APTX_HD_DECODE
	&a2dp_aptx_hd_sink,
# endif
#endif
#if ENABLE_APTX
	&a2dp_aptx_source,
# if HAVE_APTX_DECODE
	&a2dp_aptx_sink,
# endif
#endif
#if ENABLE_FASTSTREAM
	&a2dp_faststream_source,
	&a2dp_faststream_sink,
#endif
#if ENABLE_AAC
	&a2dp_aac_source,
	&a2dp_aac_sink,
#endif
#if ENABLE_MPEG
# if ENABLE_MP3LAME
	&a2dp_mpeg_source,
# endif
# if ENABLE_MP3LAME || ENABLE_MPG123
	&a2dp_mpeg_sink,
# endif
#endif
	&a2dp_sbc_source,
	&a2dp_sbc_sink,
	NULL,
};

/**
 * Initialize A2DP codecs. */
int a2dp_codecs_init(void) {

	for (size_t i = 0; a2dp_codecs[i] != NULL; i++) {
		/* We want the list of codecs to be seen as const outside
		 * of this file, so we have to cast it here. */
		struct a2dp_codec *c = (struct a2dp_codec *)a2dp_codecs[i];

		switch (c->dir) {
		case A2DP_SOURCE:
			c->enabled &= config.profile.a2dp_source;
			break;
		case A2DP_SINK:
			c->enabled &= config.profile.a2dp_sink;
			break;
		}

		if (c->init != NULL && c->enabled)
			if (c->init(c) != 0)
				return -1;

	}

	return 0;
}

static int a2dp_codec_id_cmp(uint16_t a, uint16_t b) {
	if (a < A2DP_CODEC_VENDOR || b < A2DP_CODEC_VENDOR)
		return a - b;
	const char *a_name;
	if ((a_name = a2dp_codecs_codec_id_to_string(a)) == NULL)
		return 1;
	const char *b_name;
	if ((b_name = a2dp_codecs_codec_id_to_string(b)) == NULL)
		return -1;
	return strcasecmp(a_name, b_name);
}

/**
 * Compare A2DP codecs.
 *
 * This function orders A2DP codecs according to following rules:
 *  - order codecs by A2DP direction
 *  - order codecs by codec ID
 *  - order vendor codecs alphabetically (case insensitive) */
int a2dp_codec_cmp(const struct a2dp_codec *a, const struct a2dp_codec *b) {
	if (a->dir == b->dir)
		return a2dp_codec_id_cmp(a->codec_id, b->codec_id);
	return a->dir - b->dir;
}

/**
 * Compare A2DP codecs. */
int a2dp_codec_ptr_cmp(const struct a2dp_codec **a, const struct a2dp_codec **b) {
	return a2dp_codec_cmp(*a, *b);
}

/**
 * Compare A2DP SEPs. */
int a2dp_sep_cmp(const struct a2dp_sep *a, const struct a2dp_sep *b) {
	if (a->dir == b->dir)
		return a2dp_codec_id_cmp(a->codec_id, b->codec_id);
	return a->dir - b->dir;
}

/**
 * Lookup codec configuration for given stream direction.
 *
 * @param codec_id BlueALSA A2DP 16-bit codec ID.
 * @param dir The A2DP stream direction.
 * @return On success this function returns the address of the codec
 *   configuration structure. Otherwise, NULL is returned. */
const struct a2dp_codec *a2dp_codec_lookup(uint16_t codec_id, enum a2dp_dir dir) {
	for (size_t i = 0; a2dp_codecs[i] != NULL; i++)
		if (a2dp_codecs[i]->dir == dir &&
				a2dp_codecs[i]->codec_id == codec_id)
			return a2dp_codecs[i];
	return NULL;
}

/**
 * Lookup number of channels for given capability value.
 *
 * @param codec A2DP codec setup.
 * @param capability_value A2DP codec channel mode configuration value.
 * @param backchannel If true, lookup in the back-channel configuration.
 * @return On success this function returns the number of channels. Otherwise,
 *   if given capability value is not supported (or invalid), 0 is returned. */
unsigned int a2dp_codec_lookup_channels(
		const struct a2dp_codec *codec,
		uint16_t capability_value,
		bool backchannel) {

	const size_t slot = backchannel ? 1 : 0;
	size_t i;

	for (i = 0; i < codec->channels_size[slot]; i++)
		if (capability_value == codec->channels[slot][i].value)
			return codec->channels[slot][i].channels;

	return 0;
}

/**
 * Lookup sampling frequency for given capability value.
 *
 * @param codec A2DP codec setup.
 * @param capability_value A2DP codec sampling configuration value.
 * @param backchannel If true, lookup in the back-channel configuration.
 * @return On success this function returns the sampling frequency. Otherwise,
 *   if given capability value is not supported (or invalid), 0 is returned. */
unsigned int a2dp_codec_lookup_frequency(
		const struct a2dp_codec *codec,
		uint16_t capability_value,
		bool backchannel) {

	const size_t slot = backchannel ? 1 : 0;
	size_t i;

	for (i = 0; i < codec->samplings_size[slot]; i++)
		if (capability_value == codec->samplings[slot][i].value)
			return codec->samplings[slot][i].frequency;

	return 0;
}

/**
 * Get A2DP 16-bit vendor codec ID - BlueALSA extension.
 *
 * @param capabilities A2DP vendor codec capabilities.
 * @param size A2DP vendor codec capabilities size.
 * @return On success this function returns A2DP 16-bit vendor codec ID. */
uint16_t a2dp_get_vendor_codec_id(const void *capabilities, size_t size) {

	if (size < sizeof(a2dp_vendor_codec_t))
		return errno = EINVAL, 0xFFFF;

	uint32_t vendor_id = A2DP_GET_VENDOR_ID(*(a2dp_vendor_codec_t *)capabilities);
	uint16_t codec_id = A2DP_GET_CODEC_ID(*(a2dp_vendor_codec_t *)capabilities);

	switch (vendor_id) {
	case BT_COMPID_QUALCOMM_TECH_INTL:
		switch (codec_id) {
		case FASTSTREAM_CODEC_ID:
			return A2DP_CODEC_VENDOR_FASTSTREAM;
		case APTX_LL_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX_LL;
		} break;
	case BT_COMPID_APPLE:
		switch (codec_id) {
		} break;
	case BT_COMPID_APT:
		switch (codec_id) {
		case APTX_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX;
		} break;
	case BT_COMPID_SAMSUNG_ELEC:
		switch (codec_id) {
		case SAMSUNG_HD_CODEC_ID:
			return A2DP_CODEC_VENDOR_SAMSUNG_HD;
		case SAMSUNG_SC_CODEC_ID:
			return A2DP_CODEC_VENDOR_SAMSUNG_SC;
		} break;
	case BT_COMPID_QUALCOMM_TECH:
		switch (codec_id) {
		case APTX_HD_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX_HD;
		case APTX_TWS_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX_TWS;
		case APTX_AD_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX_AD;
		} break;
	case BT_COMPID_SONY:
		switch (codec_id) {
		case LDAC_CODEC_ID:
			return A2DP_CODEC_VENDOR_LDAC;
		} break;
	case BT_COMPID_SAVITECH:
		switch (codec_id) {
		case LHDC_V1_CODEC_ID:
			return A2DP_CODEC_VENDOR_LHDC_V1;
		case LHDC_V2_CODEC_ID:
			return A2DP_CODEC_VENDOR_LHDC_V2;
		case LHDC_V3_CODEC_ID:
			return A2DP_CODEC_VENDOR_LHDC_V3;
		case LHDC_V5_CODEC_ID:
			return A2DP_CODEC_VENDOR_LHDC_V5;
		case LHDC_LL_CODEC_ID:
			return A2DP_CODEC_VENDOR_LHDC_LL;
		} break;
	case BT_COMPID_FRAUNHOFER_IIS:
		switch (codec_id) {
		case LC3PLUS_CODEC_ID:
			return A2DP_CODEC_VENDOR_LC3PLUS;
		} break;
	}

	hexdump("Unknown vendor codec", capabilities, size, true);

	errno = ENOTSUP;
	return 0xFFFF;
}

/**
 * Check whether channel mode configuration is valid. */
static bool a2dp_codec_check_channel_mode(
		const struct a2dp_codec *codec,
		unsigned int capabilities,
		bool backchannel) {

	const size_t slot = backchannel ? 1 : 0;
	size_t i;

	if (codec->channels_size[slot] == 0)
		return true;

	for (i = 0; i < codec->channels_size[slot]; i++)
		if (capabilities == codec->channels[slot][i].value)
			return true;

	return false;
}

/**
 * Check whether sampling frequency configuration is valid. */
static bool a2dp_codec_check_sampling_freq(
		const struct a2dp_codec *codec,
		unsigned int capabilities,
		bool backchannel) {

	const size_t slot = backchannel ? 1 : 0;
	size_t i;

	if (codec->samplings_size[slot] == 0)
		return true;

	for (i = 0; i < codec->samplings_size[slot]; i++)
		if (capabilities == codec->samplings[slot][i].value)
			return true;

	return false;
}

/**
 * Check whether A2DP configuration is valid.
 *
 * @param codec A2DP codec setup.
 * @param configuration A2DP codec configuration blob.
 * @param size The size of the A2DP codec configuration blob.
 * @return On success this function returns A2DP_CHECK_OK. Otherwise,
 *   A2DP_CHECK_ERR_* error bit-mask is returned to indicate invalid
 *   A2DP configuration blob. */
uint32_t a2dp_check_configuration(
		const struct a2dp_codec *codec,
		const void *configuration,
		size_t size) {

	unsigned int cap_chm = 0, cap_chm_bc = 0;
	unsigned int cap_freq = 0, cap_freq_bc = 0;
	uint32_t ret = A2DP_CHECK_OK;

	/* prevent out-of-bounds memory access */
	if (size != codec->capabilities_size)
		return A2DP_CHECK_ERR_SIZE;

	switch (codec->codec_id) {
	case A2DP_CODEC_SBC: {

		const a2dp_sbc_t *cap = configuration;
		cap_chm = cap->channel_mode;
		cap_freq = cap->frequency;

		if (cap->allocation_method != SBC_ALLOCATION_SNR &&
				cap->allocation_method != SBC_ALLOCATION_LOUDNESS) {
			debug("Invalid SBC allocation method: %#x", cap->allocation_method);
			ret |= A2DP_CHECK_ERR_SBC_ALLOCATION;
		}

		if (cap->subbands != SBC_SUBBANDS_4 &&
				cap->subbands != SBC_SUBBANDS_8) {
			debug("Invalid SBC sub-bands: %#x", cap->subbands);
			ret |= A2DP_CHECK_ERR_SBC_SUB_BANDS;
		}

		if (cap->block_length != SBC_BLOCK_LENGTH_4 &&
				cap->block_length != SBC_BLOCK_LENGTH_8 &&
				cap->block_length != SBC_BLOCK_LENGTH_12 &&
				cap->block_length != SBC_BLOCK_LENGTH_16) {
			debug("Invalid SBC block length: %#x", cap->block_length);
			ret |= A2DP_CHECK_ERR_SBC_BLOCK_LENGTH;
		}

		debug("Selected A2DP SBC bit-pool range: [%u, %u]",
				cap->min_bitpool, cap->max_bitpool);

		break;
	}

#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12: {

		const a2dp_mpeg_t *cap = configuration;
		cap_chm = cap->channel_mode;
		cap_freq = cap->frequency;

		if (cap->layer != MPEG_LAYER_MP1 &&
				cap->layer != MPEG_LAYER_MP2 &&
				cap->layer != MPEG_LAYER_MP3) {
			debug("Invalid MPEG layer: %#x", cap->layer);
			ret |= A2DP_CHECK_ERR_MPEG_LAYER;
		}

		break;
	}
#endif

#if ENABLE_AAC
	case A2DP_CODEC_MPEG24: {

		const a2dp_aac_t *cap = configuration;
		cap_chm = cap->channels;
		cap_freq = AAC_GET_FREQUENCY(*cap);

		if (cap->object_type != AAC_OBJECT_TYPE_MPEG2_AAC_LC &&
				cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_LC &&
				cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_LTP &&
				cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_SCA) {
			debug("Invalid AAC object type: %#x", cap->object_type);
			ret |= A2DP_CHECK_ERR_AAC_OBJ_TYPE;
		}

		break;
	}
#endif

#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX: {
		const a2dp_aptx_t *cap = configuration;
		cap_chm = cap->channel_mode;
		cap_freq = cap->frequency;
		break;
	}
#endif

#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD: {
		const a2dp_aptx_hd_t *cap = configuration;
		cap_chm = cap->aptx.channel_mode;
		cap_freq = cap->aptx.frequency;
		break;
	}
#endif

#if ENABLE_FASTSTREAM
	case A2DP_CODEC_VENDOR_FASTSTREAM: {

		const a2dp_faststream_t *cap = configuration;
		cap_freq = cap->frequency_music;
		cap_freq_bc = cap->frequency_voice;

		if ((cap->direction & (FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE)) == 0) {
			debug("Invalid FastStream directions: %#x", cap->direction);
			ret |= A2DP_CHECK_ERR_FASTSTREAM_DIR;
		}

		break;
	}
#endif

#if ENABLE_LC3PLUS
	case A2DP_CODEC_VENDOR_LC3PLUS: {

		const a2dp_lc3plus_t *cap = configuration;
		cap_chm = cap->channels;
		cap_freq = LC3PLUS_GET_FREQUENCY(*cap);

		if (cap->frame_duration != LC3PLUS_FRAME_DURATION_025 &&
				cap->frame_duration != LC3PLUS_FRAME_DURATION_050 &&
				cap->frame_duration != LC3PLUS_FRAME_DURATION_100) {
			debug("Invalid LC3plus frame duration: %#x", cap->frame_duration);
			ret |= A2DP_CHECK_ERR_LC3PLUS_DURATION;
		}

		break;
	}
#endif

#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC: {
		const a2dp_ldac_t *cap = configuration;
		cap_chm = cap->channel_mode;
		cap_freq = cap->frequency;
		break;
	}
#endif

	default:
		g_assert_not_reached();
	}

	if (!a2dp_codec_check_channel_mode(codec, cap_chm, false)) {
		debug("Invalid channel mode: %#x", cap_chm);
		ret |= A2DP_CHECK_ERR_CHANNELS;
	}

	if (!a2dp_codec_check_channel_mode(codec, cap_chm_bc, true)) {
		debug("Invalid back-channel channel mode: %#x", cap_chm_bc);
		ret |= A2DP_CHECK_ERR_CHANNELS_BC;
	}

	if (!a2dp_codec_check_sampling_freq(codec, cap_freq, false)) {
		debug("Invalid sampling frequency: %#x", cap_freq);
		ret |= A2DP_CHECK_ERR_SAMPLING;
	}

	if (!a2dp_codec_check_sampling_freq(codec, cap_freq_bc, true)) {
		debug("Invalid back-channel sampling frequency: %#x", cap_freq_bc);
		ret |= A2DP_CHECK_ERR_SAMPLING_BC;
	}

	return ret;
}

/**
 * Narrow A2DP codec capabilities to values supported by BlueALSA. */
int a2dp_filter_capabilities(
		const struct a2dp_codec *codec,
		void *capabilities,
		size_t size) {

	if (size != codec->capabilities_size) {
		error("Invalid capabilities size: %zu != %zu", size, codec->capabilities_size);
		return errno = EINVAL, -1;
	}

	a2dp_t tmp;
	g_assert_cmpuint(sizeof(tmp), >=, size);

	size_t i;
	for (i = 0; i < size; i++)
		((uint8_t *)&tmp)[i] = ((uint8_t *)capabilities)[i] & ((uint8_t *)&codec->capabilities)[i];

	switch (codec->codec_id) {
	case A2DP_CODEC_SBC:
		tmp.sbc.min_bitpool = MAX(
			((a2dp_sbc_t *)capabilities)->min_bitpool,
			codec->capabilities.sbc.min_bitpool);
		tmp.sbc.max_bitpool = MIN(
			((a2dp_sbc_t *)capabilities)->max_bitpool,
			codec->capabilities.sbc.max_bitpool);
		break;
#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12:
		break;
#endif
#if ENABLE_AAC
	case A2DP_CODEC_MPEG24:
		AAC_SET_BITRATE(tmp.aac, MIN(
					AAC_GET_BITRATE(*(a2dp_aac_t *)capabilities),
					AAC_GET_BITRATE(codec->capabilities.aac)));
		break;
#endif
#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX:
		break;
#endif
#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD:
		break;
#endif
#if ENABLE_FASTSTREAM
	case A2DP_CODEC_VENDOR_FASTSTREAM:
		break;
#endif
#if ENABLE_LC3PLUS
	case A2DP_CODEC_VENDOR_LC3PLUS:
		break;
#endif
#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC:
		break;
#endif
	default:
		g_assert_not_reached();
	}

	memcpy(capabilities, &tmp, size);
	return 0;
}

/**
 * Select (best) channel mode configuration. */
static unsigned int a2dp_codec_select_channel_mode(
		const struct a2dp_codec *codec,
		unsigned int capabilities,
		bool backchannel) {

	const size_t slot = backchannel ? 1 : 0;
	size_t i;

	/* If monophonic sound has been forced, check whether given codec supports
	 * such a channel mode. Since mono channel mode shall be stored at index 0
	 * we can simply check for its existence with a simple index lookup. */
	if (config.a2dp.force_mono &&
			codec->channels[slot][0].mode == A2DP_CHM_MONO &&
			capabilities & codec->channels[slot][0].value)
		return codec->channels[slot][0].value;

	/* favor higher number of channels */
	for (i = codec->channels_size[slot]; i > 0; i--)
		if (capabilities & codec->channels[slot][i - 1].value)
			return codec->channels[slot][i - 1].value;

	return 0;
}

/**
 * Select (best) sampling frequency configuration. */
static unsigned int a2dp_codec_select_sampling_freq(
		const struct a2dp_codec *codec,
		unsigned int capabilities,
		bool backchannel) {

	const size_t slot = backchannel ? 1 : 0;
	size_t i;

	if (config.a2dp.force_44100)
		for (i = 0; i < codec->samplings_size[slot]; i++)
			if (codec->samplings[slot][i].frequency == 44100) {
				if (capabilities & codec->samplings[slot][i].value)
					return codec->samplings[slot][i].value;
				break;
			}

	/* favor higher sampling frequencies */
	for (i = codec->samplings_size[slot]; i > 0; i--)
		if (capabilities & codec->samplings[slot][i - 1].value)
			return codec->samplings[slot][i - 1].value;

	return 0;
}

/**
 * Select (best) A2DP codec configuration. */
int a2dp_select_configuration(
		const struct a2dp_codec *codec,
		void *capabilities,
		size_t size) {

	if (size != codec->capabilities_size) {
		error("Invalid capabilities size: %zu != %zu", size, codec->capabilities_size);
		return errno = EINVAL, -1;
	}

	a2dp_t tmp;
	/* save original capabilities blob for later */
	memcpy(&tmp, capabilities, size);

	/* Narrow capabilities to values supported by BlueALSA. */
	if (a2dp_filter_capabilities(codec, capabilities, size) != 0)
		return -1;

	switch (codec->codec_id) {
	case A2DP_CODEC_SBC: {
		a2dp_sbc_t *cap = capabilities;

		const unsigned int cap_chm = cap->channel_mode;
		if ((cap->channel_mode = a2dp_codec_select_channel_mode(codec, cap_chm, false)) == 0) {
			error("SBC: No supported channel modes: %#x", tmp.sbc.channel_mode);
			goto fail;
		}

		if (config.sbc_quality == SBC_QUALITY_XQ ||
				config.sbc_quality == SBC_QUALITY_XQPLUS) {
			if (cap_chm & SBC_CHANNEL_MODE_DUAL_CHANNEL)
				cap->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
			else
				warn("SBC XQ: Dual channel mode not supported: %#x", tmp.sbc.channel_mode);
		}

		const unsigned int cap_freq = cap->frequency;
		if ((cap->frequency = a2dp_codec_select_sampling_freq(codec, cap_freq, false)) == 0) {
			error("SBC: No supported sampling frequencies: %#x", tmp.sbc.frequency);
			goto fail;
		}

		const uint8_t cap_block_length = cap->block_length;
		if (cap_block_length & SBC_BLOCK_LENGTH_16)
			cap->block_length = SBC_BLOCK_LENGTH_16;
		else if (cap_block_length & SBC_BLOCK_LENGTH_12)
			cap->block_length = SBC_BLOCK_LENGTH_12;
		else if (cap_block_length & SBC_BLOCK_LENGTH_8)
			cap->block_length = SBC_BLOCK_LENGTH_8;
		else if (cap_block_length & SBC_BLOCK_LENGTH_4)
			cap->block_length = SBC_BLOCK_LENGTH_4;
		else {
			error("SBC: No supported block lengths: %#x", tmp.sbc.block_length);
			goto fail;
		}

		const uint8_t cap_subbands = cap->subbands;
		if (cap_subbands & SBC_SUBBANDS_8)
			cap->subbands = SBC_SUBBANDS_8;
		else if (cap_subbands & SBC_SUBBANDS_4)
			cap->subbands = SBC_SUBBANDS_4;
		else {
			error("SBC: No supported sub-bands: %#x", tmp.sbc.subbands);
			goto fail;
		}

		const uint8_t cap_allocation_method = cap->allocation_method;
		if (cap_allocation_method & SBC_ALLOCATION_LOUDNESS)
			cap->allocation_method = SBC_ALLOCATION_LOUDNESS;
		else if (cap_allocation_method & SBC_ALLOCATION_SNR)
			cap->allocation_method = SBC_ALLOCATION_SNR;
		else {
			error("SBC: No supported allocation method: %#x", tmp.sbc.allocation_method);
			goto fail;
		}

		break;
	}

#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12: {
		a2dp_mpeg_t *cap = capabilities;

		const uint8_t cap_layer = cap->layer;
		if (cap_layer & MPEG_LAYER_MP3)
			cap->layer = MPEG_LAYER_MP3;
		else if (cap_layer & MPEG_LAYER_MP2)
			cap->layer = MPEG_LAYER_MP2;
		else if (cap_layer & MPEG_LAYER_MP1)
			cap->layer = MPEG_LAYER_MP1;
		else {
			error("MPEG: No supported layer: %#x", tmp.mpeg.layer);
			goto fail;
		}

		const unsigned int cap_chm = cap->channel_mode;
		if ((cap->channel_mode = a2dp_codec_select_channel_mode(codec, cap_chm, false)) == 0) {
			error("MPEG: No supported channel modes: %#x", tmp.mpeg.channel_mode);
			goto fail;
		}

		const unsigned int cap_freq = cap->frequency;
		if ((cap->frequency = a2dp_codec_select_sampling_freq(codec, cap_freq, false)) == 0) {
			error("MPEG: No supported sampling frequencies: %#x", tmp.mpeg.frequency);
			goto fail;
		}

		/* do not waste bits for CRC protection */
		cap->crc = 0;
		/* do not use MPF-2 */
		cap->mpf = 0;

		break;
	}
#endif

#if ENABLE_AAC
	case A2DP_CODEC_MPEG24: {
		a2dp_aac_t *cap = capabilities;

		const uint8_t cap_object_type = cap->object_type;
		if (cap_object_type & AAC_OBJECT_TYPE_MPEG4_AAC_SCA)
			cap->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_SCA;
		else if (cap_object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LTP)
			cap->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LTP;
		else if (cap_object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC)
			cap->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC;
		else if (cap_object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC)
			cap->object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC;
		else {
			error("AAC: No supported object type: %#x", tmp.aac.object_type);
			goto fail;
		}

		const unsigned int cap_chm = cap->channels;
		if ((cap->channels = a2dp_codec_select_channel_mode(codec, cap_chm, false)) == 0) {
			error("AAC: No supported channels: %#x", tmp.aac.channels);
			goto fail;
		}

		unsigned int freq;
		const unsigned int cap_freq = AAC_GET_FREQUENCY(*cap);
		if ((freq = a2dp_codec_select_sampling_freq(codec, cap_freq, false)) != 0)
			AAC_SET_FREQUENCY(*cap, freq);
		else {
			error("AAC: No supported sampling frequencies: %#x", AAC_GET_FREQUENCY(tmp.aac));
			goto fail;
		}

		unsigned int ba_bitrate = AAC_GET_BITRATE(codec->capabilities.aac);
		unsigned int peer_bitrate = AAC_GET_BITRATE(*cap);
		if (peer_bitrate == 0)
			/* fix bitrate value if it was not set */
			peer_bitrate = UINT_MAX;
		AAC_SET_BITRATE(*cap, MIN(ba_bitrate, peer_bitrate));

		if (!config.aac_prefer_vbr)
			cap->vbr = 0;

		break;
	}
#endif

#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX: {
		a2dp_aptx_t *cap = capabilities;

		const unsigned int cap_chm = cap->channel_mode;
		if ((cap->channel_mode = a2dp_codec_select_channel_mode(codec, cap_chm, false)) == 0) {
			error("apt-X: No supported channel modes: %#x", tmp.aptx.channel_mode);
			goto fail;
		}

		const unsigned int cap_freq = cap->frequency;
		if ((cap->frequency = a2dp_codec_select_sampling_freq(codec, cap_freq, false)) == 0) {
			error("apt-X: No supported sampling frequencies: %#x", tmp.aptx.frequency);
			goto fail;
		}

		break;
	}
#endif

#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD: {
		a2dp_aptx_hd_t *cap = capabilities;

		const unsigned int cap_chm = cap->aptx.channel_mode;
		if ((cap->aptx.channel_mode = a2dp_codec_select_channel_mode(codec, cap_chm, false)) == 0) {
			error("apt-X HD: No supported channel modes: %#x", tmp.aptx_hd.aptx.channel_mode);
			goto fail;
		}

		const unsigned int cap_freq = cap->aptx.frequency;
		if ((cap->aptx.frequency = a2dp_codec_select_sampling_freq(codec, cap_freq, false)) == 0) {
			error("apt-X HD: No supported sampling frequencies: %#x", tmp.aptx_hd.aptx.frequency);
			goto fail;
		}

		break;
	}
#endif

#if ENABLE_FASTSTREAM
	case A2DP_CODEC_VENDOR_FASTSTREAM: {
		a2dp_faststream_t *cap = capabilities;

		const unsigned int cap_freq = cap->frequency_music;
		if ((cap->frequency_music = a2dp_codec_select_sampling_freq(codec, cap_freq, false)) == 0) {
			error("FastStream: No supported sampling frequencies: %#x",
					tmp.faststream.frequency_music);
			goto fail;
		}

		const unsigned int cap_freq_bc = cap->frequency_voice;
		if ((cap->frequency_voice = a2dp_codec_select_sampling_freq(codec, cap_freq_bc, true)) == 0) {
			error("FastStream: No supported back-channel sampling frequencies: %#x",
					tmp.faststream.frequency_voice);
			goto fail;
		}

		if ((cap->direction & (FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE)) == 0) {
			error("FastStream: No supported directions: %#x", tmp.faststream.direction);
		}

		break;
	}
#endif

#if ENABLE_LC3PLUS
	case A2DP_CODEC_VENDOR_LC3PLUS: {
		a2dp_lc3plus_t *cap = capabilities;

		const uint8_t cap_frame_duration = cap->frame_duration;
		if (cap_frame_duration & LC3PLUS_FRAME_DURATION_100)
			cap->frame_duration = LC3PLUS_FRAME_DURATION_100;
		else if (cap_frame_duration & LC3PLUS_FRAME_DURATION_050)
			cap->frame_duration = LC3PLUS_FRAME_DURATION_050;
		else if (cap_frame_duration & LC3PLUS_FRAME_DURATION_025)
			cap->frame_duration = LC3PLUS_FRAME_DURATION_025;
		else {
			error("LC3plus: No supported frame duration: %#x", tmp.lc3plus.frame_duration);
			goto fail;
		}

		const unsigned int cap_chm = cap->channels;
		if ((cap->channels = a2dp_codec_select_channel_mode(codec, cap_chm, false)) == 0) {
			error("LC3plus: No supported channels: %#x", tmp.lc3plus.channels);
			goto fail;
		}

		unsigned int freq;
		const unsigned int cap_freq = LC3PLUS_GET_FREQUENCY(*cap);
		if ((freq = a2dp_codec_select_sampling_freq(codec, cap_freq, false)) != 0)
			LC3PLUS_SET_FREQUENCY(*cap, freq);
		else {
			error("LC3plus: No supported sampling frequencies: %#x",
					LC3PLUS_GET_FREQUENCY(tmp.lc3plus));
			goto fail;
		}

		break;
	}
#endif

#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC: {
		a2dp_ldac_t *cap = capabilities;

		const unsigned int cap_chm = cap->channel_mode;
		if ((cap->channel_mode = a2dp_codec_select_channel_mode(codec, cap_chm, false)) == 0) {
			error("LDAC: No supported channel modes: %#x", tmp.ldac.channel_mode);
			goto fail;
		}

		const unsigned int cap_freq = cap->frequency;
		if ((cap->frequency = a2dp_codec_select_sampling_freq(codec, cap_freq, false)) == 0) {
			error("LDAC: No supported sampling frequencies: %#x", tmp.ldac.frequency);
			goto fail;
		}

		break;
	}
#endif

	default:
		g_assert_not_reached();
	}

	return 0;

fail:
	errno = ENOTSUP;
	return -1;
}

int a2dp_transport_init(
		struct ba_transport *t) {
	return t->a2dp.codec->transport_init(t);
}

int a2dp_transport_start(
		struct ba_transport *t) {
	return t->a2dp.codec->transport_start(t);
}
