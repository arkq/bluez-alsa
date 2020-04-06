/*
 * BlueALSA - bluez-a2dp.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEZA2DP_H_
#define BLUEALSA_BLUEZA2DP_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum bluez_a2dp_dir {
	BLUEZ_A2DP_SOURCE,
	BLUEZ_A2DP_SINK,
};

enum bluez_a2dp_chm {
	BLUEZ_A2DP_CHM_MONO = 0,
	/* fixed bit-rate for each channel */
	BLUEZ_A2DP_CHM_DUAL_CHANNEL,
	/* channel bits allocated dynamically */
	BLUEZ_A2DP_CHM_STEREO,
	/* L+R (mid) and L-R (side) encoding */
	BLUEZ_A2DP_CHM_JOINT_STEREO,
};

struct bluez_a2dp_channel_mode {
	enum bluez_a2dp_chm mode;
	unsigned int channels;
	uint16_t value;
};

struct bluez_a2dp_sampling_freq {
	unsigned int frequency;
	uint16_t value;
};

struct bluez_a2dp_codec {
	enum bluez_a2dp_dir dir;
	uint16_t codec_id;
	/* support for A2DP back-channel */
	bool backchannel;
	/* capabilities configuration element */
	const void *capabilities;
	size_t capabilities_size;
	/* list of supported channel modes */
	const struct bluez_a2dp_channel_mode *channels[2];
	size_t channels_size[2];
	/* list of supported sampling frequencies */
	const struct bluez_a2dp_sampling_freq *samplings[2];
	size_t samplings_size[2];
};

/**
 * A2DP Stream End-Point. */
struct bluez_a2dp_sep {
	enum bluez_a2dp_dir dir;
	uint16_t codec_id;
	/* exposed capabilities */
	void *capabilities;
	size_t capabilities_size;
	/* stream end-point path */
	char bluez_dbus_path[64];
};

/* NULL-terminated list of available A2DP codecs */
extern const struct bluez_a2dp_codec **bluez_a2dp_codecs;

#endif
