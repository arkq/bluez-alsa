/*
 * BlueALSA - a2dp.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_A2DP_H_
#define BLUEALSA_A2DP_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum a2dp_dir {
	A2DP_SOURCE = 0,
	A2DP_SINK = !A2DP_SOURCE,
};

enum a2dp_chm {
	A2DP_CHM_MONO = 0,
	/* fixed bit-rate for each channel */
	A2DP_CHM_DUAL_CHANNEL,
	/* channel bits allocated dynamically */
	A2DP_CHM_STEREO,
	/* L+R (mid) and L-R (side) encoding */
	A2DP_CHM_JOINT_STEREO,
};

struct a2dp_channel_mode {
	enum a2dp_chm mode;
	unsigned int channels;
	uint16_t value;
};

struct a2dp_sampling_freq {
	unsigned int frequency;
	uint16_t value;
};

struct a2dp_codec {
	enum a2dp_dir dir;
	uint16_t codec_id;
	/* support for A2DP back-channel */
	bool backchannel;
	/* capabilities configuration element */
	const void *capabilities;
	size_t capabilities_size;
	/* list of supported channel modes */
	const struct a2dp_channel_mode *channels[2];
	size_t channels_size[2];
	/* list of supported sampling frequencies */
	const struct a2dp_sampling_freq *samplings[2];
	size_t samplings_size[2];
};

/**
 * A2DP Stream End-Point. */
struct a2dp_sep {
	enum a2dp_dir dir;
	uint16_t codec_id;
	/* exposed capabilities */
	void *capabilities;
	size_t capabilities_size;
	/* stream end-point path */
	char bluez_dbus_path[64];
	/* selected configuration */
	void *configuration;
};

/* NULL-terminated list of available A2DP codecs */
extern const struct a2dp_codec *a2dp_codecs[];

const struct a2dp_codec *a2dp_codec_lookup(
		uint16_t codec_id,
		enum a2dp_dir dir);

unsigned int a2dp_codec_lookup_channels(
		const struct a2dp_codec *codec,
		uint16_t capability_value,
		bool backchannel);

unsigned int a2dp_codec_lookup_frequency(
		const struct a2dp_codec *codec,
		uint16_t capability_value,
		bool backchannel);

uint16_t a2dp_get_vendor_codec_id(
		const void *capabilities,
		size_t size);

#define A2DP_CHECK_OK                    0
#define A2DP_CHECK_ERR_SIZE              0xFFFFFFFF
#define A2DP_CHECK_ERR_CHANNELS          (1 << 0)
#define A2DP_CHECK_ERR_CHANNELS_BC       (1 << 1)
#define A2DP_CHECK_ERR_SAMPLING          (1 << 2)
#define A2DP_CHECK_ERR_SAMPLING_BC       (1 << 3)
#define A2DP_CHECK_ERR_SBC_ALLOCATION    (1 << 4)
#define A2DP_CHECK_ERR_SBC_SUB_BANDS     (1 << 5)
#define A2DP_CHECK_ERR_SBC_BLOCK_LENGTH  (1 << 6)
#define A2DP_CHECK_ERR_MPEG_LAYER        (1 << 7)
#define A2DP_CHECK_ERR_AAC_OBJ_TYPE      (1 << 8)

uint32_t a2dp_check_configuration(
		const struct a2dp_codec *codec,
		const void *configuration,
		size_t size);

int a2dp_filter_capabilities(
		const struct a2dp_codec *codec,
		void *capabilities,
		size_t size);

int a2dp_select_configuration(
		const struct a2dp_codec *codec,
		void *capabilities,
		size_t size);

#endif
