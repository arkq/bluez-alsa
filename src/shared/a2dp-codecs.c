/*
 * BlueALSA - a2dp-codecs.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/a2dp-codecs.h"

#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "shared/defs.h"

static const struct {
	uint16_t codec_id;
	const char *aliases[3];
} codecs[] = {
	{ A2DP_CODEC_SBC, { "SBC" } },
	{ A2DP_CODEC_MPEG12, { "MP3", "MPEG12", "MPEG" } },
	{ A2DP_CODEC_MPEG24, { "AAC", "MPEG24" } },
	{ A2DP_CODEC_MPEGD, { "USAC", "MPEG-D" } },
	{ A2DP_CODEC_ATRAC, { "ATRAC" } },
	{ A2DP_CODEC_VENDOR_APTX, { "aptX", "apt-X" } },
	{ A2DP_CODEC_VENDOR_APTX_AD, { "aptX-AD", "apt-X-AD" } },
	{ A2DP_CODEC_VENDOR_APTX_HD, { "aptX-HD", "apt-X-HD" } },
	{ A2DP_CODEC_VENDOR_APTX_LL, { "aptX-LL", "apt-X-LL" } },
	{ A2DP_CODEC_VENDOR_APTX_TWS, { "aptX-TWS", "apt-X-TWS" } },
	{ A2DP_CODEC_VENDOR_FASTSTREAM, { "FastStream", "FS" } },
	{ A2DP_CODEC_VENDOR_LC3PLUS, { "LC3plus" } },
	{ A2DP_CODEC_VENDOR_LDAC, { "LDAC" } },
	{ A2DP_CODEC_VENDOR_LHDC_V1, { "LHDC-v1" } },
	{ A2DP_CODEC_VENDOR_LHDC_V2, { "LHDC-V2" } },
	{ A2DP_CODEC_VENDOR_LHDC_V3, { "LHDC-V3", "LHDC-V4", "LLAC" } },
	{ A2DP_CODEC_VENDOR_LHDC_V5, { "LHDC-V5" } },
	{ A2DP_CODEC_VENDOR_LHDC_LL, { "LHDC-LL"} },
	{ A2DP_CODEC_VENDOR_OPUS, { "Opus"} },
	{ A2DP_CODEC_VENDOR_SAMSUNG_HD, { "samsung-HD" } },
	{ A2DP_CODEC_VENDOR_SAMSUNG_SC, { "samsung-SC" } },
};

/**
 * Get BlueALSA A2DP codec ID from string representation.
 *
 * @param alias Alias of an A2DP audio codec name.
 * @return BlueALSA audio codec ID or 0xFFFF if there was no match. */
uint16_t a2dp_codecs_codec_id_from_string(const char *alias) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		for (size_t n = 0; n < ARRAYSIZE(codecs[i].aliases); n++)
			if (codecs[i].aliases[n] != NULL &&
					strcasecmp(codecs[i].aliases[n], alias) == 0)
				return codecs[i].codec_id;
	return 0xFFFF;
}

/**
 * Convert BlueALSA A2DP codec ID into a human-readable string.
 *
 * @param codec BlueALSA A2DP audio codec ID.
 * @return Human-readable string or NULL for unknown codec. */
const char *a2dp_codecs_codec_id_to_string(uint16_t codec_id) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (codecs[i].codec_id == codec_id)
			return codecs[i].aliases[0];
	return NULL;
}

/**
 * Get A2DP audio codec canonical name.
 *
 * @param alias Alias of an A2DP audio codec name.
 * @return Canonical name of the codec or passed alias string in case when
 *   there was no match. */
const char *a2dp_codecs_get_canonical_name(const char *alias) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		for (size_t n = 0; n < ARRAYSIZE(codecs[i].aliases); n++)
			if (codecs[i].aliases[n] != NULL &&
					strcasecmp(codecs[i].aliases[n], alias) == 0)
				return codecs[i].aliases[0];
	return alias;
}
