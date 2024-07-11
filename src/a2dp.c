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
#if ENABLE_OPUS
# include "a2dp-opus.h"
#endif
#include "a2dp-sbc.h"
#include "ba-config.h"
#include "ba-transport.h"
#include "shared/a2dp-codecs.h"
#include "shared/log.h"

/**
 * Callback function which returns bitmask for the best channel mode.
 *
 * Note:
 * The user data passed to a2dp_bit_mapping_foreach() function shall be
 * a pointer to an unsigned integer variable initialized to 0. */
int a2dp_foreach_get_best_channel_mode(
		struct a2dp_bit_mapping mapping,
		void *userdata) {

	unsigned int *output = userdata;

	/* Skip multi-channel modes. If desired, multi-channel mode can be selected
	 * manually by the user using the SelectCodec() D-Bus method. */
	if (mapping.value > 2 && *output != 0)
		return 1;

	*output = mapping.bit_value;

	if (config.a2dp.force_mono && mapping.value == 1)
		return 1;

	/* Keep iterating, so the last channel mode will be selected. */
	return 0;
}

/**
 * Callback function which returns bitmask for the best sampling rate.
 *
 * Note:
 * The user data passed to a2dp_bit_mapping_foreach() function shall be
 * a pointer to an unsigned integer variable initialized to 0. */
int a2dp_foreach_get_best_sampling_freq(
		struct a2dp_bit_mapping mapping,
		void *userdata) {

	unsigned int *output = userdata;

	/* Skip anything above 48000 Hz. If desired, bigger sampling rates can be
	 * selected manually by the user using the SelectCodec() D-Bus method. */
	if (mapping.value > 48000 && *output != 0)
		return 1;

	*output = mapping.bit_value;

	if (config.a2dp.force_44100 && mapping.value == 44100)
		return 1;

	/* Keep iterating, so the last sampling rate will be selected. */
	return 0;
}

/**
 * Iterate over A2DP bit-field mappings. */
int a2dp_bit_mapping_foreach(
		const struct a2dp_bit_mapping *mappings,
		uint32_t bitmask,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	int rv = -1;
	for (size_t i = 0; mappings[i].bit_value != 0; i++)
		if (mappings[i].bit_value & bitmask)
			/* stop iteration if callback returns non-zero */
			if ((rv = func(mappings[i], userdata)) != 0)
				break;
	return rv;
}

/**
 * Lookup for given bit-value in the bit mapping table.
 *
 * @param mappings Zero-terminated array of A2DP mappings.
 * @param bit_value A2DP codec bit-value to be looked up.
 * @return On success this function returns the associated value. Otherwise,
 *   0 is returned. */
unsigned int a2dp_bit_mapping_lookup(
		const struct a2dp_bit_mapping *mappings,
		uint32_t bit_value) {
	for (size_t i = 0; mappings[i].bit_value != 0; i++)
		if (mappings[i].bit_value == bit_value)
			return mappings[i].value;
	return 0;
}

struct a2dp_sep * const a2dp_seps[] = {
#if ENABLE_OPUS
	&a2dp_opus_source,
	&a2dp_opus_sink,
#endif
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
 * Initialize A2DP SEPs. */
int a2dp_seps_init(void) {

	for (size_t i = 0; a2dp_seps[i] != NULL; i++) {
		/* We want the list of SEPs to be seen as const outside
		 * of this file, so we have to cast it here. */
		struct a2dp_sep *sep = (struct a2dp_sep *)a2dp_seps[i];

		switch (sep->type) {
		case A2DP_SOURCE:
			sep->enabled &= config.profile.a2dp_source;
			break;
		case A2DP_SINK:
			sep->enabled &= config.profile.a2dp_sink;
			break;
		}

		if (sep->init != NULL && sep->enabled)
			if (sep->init(sep) != 0)
				return -1;

	}

	return 0;
}

static int a2dp_codec_id_cmp(uint32_t a, uint32_t b) {
	if (a < A2DP_CODEC_VENDOR || b < A2DP_CODEC_VENDOR)
		return a < b ? -1 : (a == b ? 0 : 1);
	const char *a_name;
	if ((a_name = a2dp_codecs_codec_id_to_string(a)) == NULL)
		return 1;
	const char *b_name;
	if ((b_name = a2dp_codecs_codec_id_to_string(b)) == NULL)
		return -1;
	return strcasecmp(a_name, b_name);
}

/**
 * Compare A2DP SEPs.
 *
 * This function orders A2DP SEPs according to following rules:
 *  - order SEPs by A2DP type
 *  - order SEPs by codec ID
 *  - order vendor codecs alphabetically (case insensitive) */
int a2dp_sep_cmp(const struct a2dp_sep *a, const struct a2dp_sep *b) {
	if (a->type == b->type)
		return a2dp_codec_id_cmp(a->codec_id, b->codec_id);
	return a->type - b->type;
}

/**
 * Compare A2DP SEPs. */
int a2dp_sep_ptr_cmp(const struct a2dp_sep **a, const struct a2dp_sep **b) {
	return a2dp_sep_cmp(*a, *b);
}

/**
 * Lookup SEP for given type and codec.
 *
 * @param type The A2DP SEP type.
 * @param codec_id BlueALSA A2DP 32-bit codec ID.
 * @return On success this function returns the address of the SEP
 *   configuration structure. Otherwise, NULL is returned. */
const struct a2dp_sep *a2dp_sep_lookup(enum a2dp_type type, uint32_t codec_id) {
	for (size_t i = 0; a2dp_seps[i] != NULL; i++)
		if (a2dp_seps[i]->type == type &&
				a2dp_seps[i]->codec_id == codec_id)
			return a2dp_seps[i];
	return NULL;
}

/**
 * Get A2DP 32-bit vendor codec ID - BlueALSA extension.
 *
 * @param capabilities A2DP vendor codec capabilities.
 * @param size A2DP vendor codec capabilities size.
 * @return On success this function returns A2DP 32-bit vendor codec ID. */
uint32_t a2dp_get_vendor_codec_id(const void *capabilities, size_t size) {

	if (size < sizeof(a2dp_vendor_info_t))
		return errno = EINVAL, 0xFFFFFFFF;

	const a2dp_vendor_info_t *info = capabilities;
	const uint32_t vendor_id = A2DP_VENDOR_INFO_GET_VENDOR_ID(*info);
	const uint16_t codec_id = A2DP_VENDOR_INFO_GET_CODEC_ID(*info);

	return A2DP_CODEC_VENDOR_ID(vendor_id, codec_id);
}

/**
 * Filter A2DP codec capabilities with given capabilities mask. */
int a2dp_filter_capabilities(
		const struct a2dp_sep *sep,
		const void *capabilities_mask,
		void *capabilities,
		size_t size) {

	if (size != sep->capabilities_size) {
		error("Invalid capabilities size: %zu != %zu", size, sep->capabilities_size);
		return errno = EINVAL, -1;
	}

	const uint8_t *caps_mask = capabilities_mask;
	uint8_t *caps = capabilities;

	if (sep->capabilities_filter != NULL)
		return sep->capabilities_filter(sep, caps_mask, caps);

	/* perform simple bitwise AND operation on given capabilities */
	for (size_t i = 0; i < sep->capabilities_size; i++)
		caps[i] = caps[i] & caps_mask[i];

	return 0;
}

/**
 * Select best possible A2DP codec configuration. */
int a2dp_select_configuration(
		const struct a2dp_sep *sep,
		void *capabilities,
		size_t size) {

	if (size == sep->capabilities_size)
		return sep->configuration_select(sep, capabilities);

	error("Invalid capabilities size: %zu != %zu", size, sep->capabilities_size);
	return errno = EINVAL, -1;
}

/**
 * Check whether A2DP configuration is valid.
 *
 * @param sep A2DP Stream End-Point setup.
 * @param configuration A2DP codec configuration blob.
 * @param size The size of the A2DP codec configuration blob.
 * @return On success this function returns A2DP_CHECK_OK. Otherwise,
 *   one of the A2DP_CHECK_ERR_* values is returned. */
enum a2dp_check_err a2dp_check_configuration(
		const struct a2dp_sep *sep,
		const void *configuration,
		size_t size) {

	if (size == sep->capabilities_size)
		return sep->configuration_check(sep, configuration);

	error("Invalid configuration size: %zu != %zu", size, sep->capabilities_size);
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
	return t->a2dp.sep->transport_init(t);
}

int a2dp_transport_start(
		struct ba_transport *t) {
	return t->a2dp.sep->transport_start(t);
}
