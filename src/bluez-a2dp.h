/*
 * BlueALSA - bluez-a2dp.h
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEZA2DP_H_
#define BLUEALSA_BLUEZA2DP_H_

#include <stddef.h>

#include "a2dp-codecs.h"

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
	uint16_t value;
};

struct bluez_a2dp_sampling_freq {
	int frequency;
	uint16_t value;
};

struct bluez_a2dp_codec {
	enum bluez_a2dp_dir dir;
	uint16_t id;
	/* capabilities configuration element */
	const void *cfg;
	size_t cfg_size;
	/* list of supported channel modes */
	const struct bluez_a2dp_channel_mode *channels;
	size_t channels_size;
	/* list of supported sampling frequencies */
	const struct bluez_a2dp_sampling_freq *samplings;
	size_t samplings_size;
};

/* NULL-terminated list of available A2DP codecs */
const struct bluez_a2dp_codec **bluez_a2dp_codecs;

#endif
