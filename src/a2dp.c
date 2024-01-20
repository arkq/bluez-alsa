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
#include <stdbool.h>
#include <strings.h>

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
 * Lookup channel mode for given configuration.
 *
 * @param channels Zero-terminated array of A2DP codec channel modes.
 * @param value A2DP codec channel mode configuration value.
 * @return On success this function returns the channel mode. Otherwise, NULL
 *  is returned. */
const struct a2dp_channels *a2dp_channels_lookup(
		const struct a2dp_channels *channels,
		uint16_t value) {
	for (size_t i = 0; channels[i].value != 0; i++)
		if (channels[i].value == value)
			return &channels[i];
	return NULL;
}

/**
 * Select channel mode based on given capabilities. */
const struct a2dp_channels *a2dp_channels_select(
		const struct a2dp_channels *channels,
		uint16_t capabilities) {

	/* If monophonic sound has been forced, check whether given codec supports
	 * such a channel mode. Since mono channel mode shall be stored at index 0
	 * we can simply check for its existence with a simple index lookup. */
	if (config.a2dp.force_mono &&
			channels[0].count == 1 &&
			capabilities & channels[0].value)
		return &channels[0];

	const struct a2dp_channels *selected = NULL;

	/* favor higher number of channels */
	for (size_t i = 0; channels[i].value != 0; i++) {
		if (channels[i].count > 2)
			/* When auto-selecting channel mode, skip multi-channel modes. If
			 * desired, multi-channel mode can be selected manually by the user
			 * using the SelectCodec() D-Bus method. */
			continue;
		if (capabilities & channels[i].value)
			selected = &channels[i];
	}

	return selected;
}

/**
 * Lookup sampling frequency for given configuration.
 *
 * @param samplings Zero-terminated array of A2DP codec sampling frequencies.
 * @param value A2DP codec sampling frequency configuration value.
 * @return On success this function returns the sampling frequency. Otherwise,
 *   NULL is returned. */
const struct a2dp_sampling *a2dp_sampling_lookup(
		const struct a2dp_sampling *samplings,
		uint16_t value) {
	for (size_t i = 0; samplings[i].value != 0; i++)
		if (samplings[i].value == value)
			return &samplings[i];
	return NULL;
}

/**
 * Select sampling frequency based on given capabilities. */
const struct a2dp_sampling *a2dp_sampling_select(
		const struct a2dp_sampling *samplings,
		uint16_t capabilities) {

	if (config.a2dp.force_44100)
		for (size_t i = 0; samplings[i].value != 0; i++)
			if (samplings[i].frequency == 44100) {
				if (capabilities & samplings[i].value)
					return &samplings[i];
				break;
			}

	const struct a2dp_sampling *selected = NULL;

	/* favor higher sampling frequencies */
	for (size_t i = 0; samplings[i].value != 0; i++)
		if (capabilities & samplings[i].value)
			selected = &samplings[i];

	return selected;
}

/**
 * Get A2DP 16-bit vendor codec ID - BlueALSA extension.
 *
 * @param capabilities A2DP vendor codec capabilities.
 * @param size A2DP vendor codec capabilities size.
 * @return On success this function returns A2DP 16-bit vendor codec ID. */
uint16_t a2dp_get_vendor_codec_id(const void *capabilities, size_t size) {

	if (size < sizeof(a2dp_vendor_info_t))
		return errno = EINVAL, 0xFFFF;

	const a2dp_vendor_info_t *info = capabilities;
	const uint32_t vendor_id = A2DP_VENDOR_INFO_GET_VENDOR_ID(*info);
	const uint16_t codec_id = A2DP_VENDOR_INFO_GET_CODEC_ID(*info);

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
	case BT_COMPID_LINUX_FOUNDATION:
		switch (codec_id) {
		case OPUS_CODEC_ID:
			return A2DP_CODEC_VENDOR_OPUS;
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
 * Filter A2DP codec capabilities with given capabilities mask. */
int a2dp_filter_capabilities(
		const struct a2dp_codec *codec,
		const void *capabilities_mask,
		void *capabilities,
		size_t size) {

	if (size != codec->capabilities_size) {
		error("Invalid capabilities size: %zu != %zu", size, codec->capabilities_size);
		return errno = EINVAL, -1;
	}

	const uint8_t *caps_mask = capabilities_mask;
	uint8_t *caps = capabilities;

	if (codec->capabilities_filter != NULL)
		return codec->capabilities_filter(codec, caps_mask, caps);

	/* perform simple bitwise AND operation on given capabilities */
	for (size_t i = 0; i < codec->capabilities_size; i++)
		caps[i] = caps[i] & caps_mask[i];

	return 0;
}

/**
 * Select best possible A2DP codec configuration. */
int a2dp_select_configuration(
		const struct a2dp_codec *codec,
		void *capabilities,
		size_t size) {

	if (size == codec->capabilities_size)
		return codec->configuration_select(codec, capabilities);

	error("Invalid capabilities size: %zu != %zu", size, codec->capabilities_size);
	return errno = EINVAL, -1;
}

/**
 * Check whether A2DP configuration is valid.
 *
 * @param codec A2DP codec setup.
 * @param configuration A2DP codec configuration blob.
 * @param size The size of the A2DP codec configuration blob.
 * @return On success this function returns A2DP_CHECK_OK. Otherwise,
 *   one of the A2DP_CHECK_ERR_* values is returned. */
enum a2dp_check_err a2dp_check_configuration(
		const struct a2dp_codec *codec,
		const void *configuration,
		size_t size) {

	if (size == codec->capabilities_size)
		return codec->configuration_check(codec, configuration);

	error("Invalid configuration size: %zu != %zu", size, codec->capabilities_size);
	return A2DP_CHECK_ERR_SIZE;
}

/**
 * Get string representation of A2DP configuration check error. */
const char *a2dp_check_strerror(
		enum a2dp_check_err err) {
	switch (err) {
	case A2DP_CHECK_OK:
		return "Success";
	case A2DP_CHECK_ERR_SIZE:
		return "Invalid size";
	case A2DP_CHECK_ERR_CHANNEL_MODE:
		return "Invalid channel mode";
	case A2DP_CHECK_ERR_SAMPLING:
		return "Invalid sampling frequency";
	case A2DP_CHECK_ERR_ALLOCATION_METHOD:
		return "Invalid allocation method";
	case A2DP_CHECK_ERR_BIT_POOL_RANGE:
		return "Invalid bit-pool range";
	case A2DP_CHECK_ERR_SUB_BANDS:
		return "Invalid sub-bands";
	case A2DP_CHECK_ERR_BLOCK_LENGTH:
		return "Invalid block length";
	case A2DP_CHECK_ERR_MPEG_LAYER:
		return "Invalid MPEG layer";
	case A2DP_CHECK_ERR_OBJECT_TYPE:
		return "Invalid object type";
	case A2DP_CHECK_ERR_DIRECTIONS:
		return "Invalid directions";
	case A2DP_CHECK_ERR_SAMPLING_VOICE:
		return "Invalid voice sampling frequency";
	case A2DP_CHECK_ERR_SAMPLING_MUSIC:
		return "Invalid music sampling frequency";
	case A2DP_CHECK_ERR_FRAME_DURATION:
		return "Invalid frame duration";
	}
	debug("Unknown error code: %#x", err);
	return "Check error";
}

int a2dp_transport_init(
		struct ba_transport *t) {
	return t->a2dp.codec->transport_init(t);
}

int a2dp_transport_start(
		struct ba_transport *t) {
	return t->a2dp.codec->transport_start(t);
}
