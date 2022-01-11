/*
 * BlueALSA - hfp.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "hfp.h"

#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "shared/defs.h"

static const struct {
	uint16_t codec_id;
	const char *aliases[1];
} codecs[] = {
	{ HFP_CODEC_CVSD, { "CVSD" } },
	{ HFP_CODEC_MSBC, { "mSBC" } },
};

/**
 * Get BlueALSA HFP codec ID from string representation.
 *
 * @param alias Alias of HFP audio codec name.
 * @return BlueALSA audio codec ID or 0xFFFF if there was no match. */
uint16_t hfp_codec_id_from_string(const char *alias) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		for (size_t n = 0; n < ARRAYSIZE(codecs[i].aliases); n++)
			if (codecs[i].aliases[n] != NULL &&
					strcasecmp(codecs[i].aliases[n], alias) == 0)
				return codecs[i].codec_id;
	return 0xFFFF;
}

/**
 * Convert BlueALSA HFP codec ID into a human-readable string.
 *
 * @param codec BlueALSA HFP audio codec ID.
 * @return Human-readable string or NULL for unknown codec. */
const char *hfp_codec_id_to_string(uint16_t codec_id) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (codecs[i].codec_id == codec_id)
			return codecs[i].aliases[0];
	return NULL;
}
