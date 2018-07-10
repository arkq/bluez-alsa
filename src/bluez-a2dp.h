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

struct bluez_a2dp_codec {
	enum bluez_a2dp_dir dir;
	uint16_t id;
	const void *cfg;
	size_t cfg_size;
};

/* NULL-terminated list of available A2DP codecs */
const struct bluez_a2dp_codec **bluez_a2dp_codecs;

#endif
