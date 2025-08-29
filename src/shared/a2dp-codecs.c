/*
 * BlueALSA - a2dp-codecs.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-codecs.h"

#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "defs.h"

static const struct {
	uint32_t codec_id;
	const char *aliases[3];
} codecs[] = {
	{ A2DP_CODEC_SBC, { "SBC" } },
	{ A2DP_CODEC_MPEG12, { "MP3", "MPEG12", "MPEG" } },
	{ A2DP_CODEC_MPEG24, { "AAC", "MPEG24" } },
	{ A2DP_CODEC_MPEGD, { "USAC", "MPEG-D" } },
	{ A2DP_CODEC_ATRAC, { "ATRAC" } },
	{ A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID), { "aptX", "apt-X" } },
	{ A2DP_CODEC_VENDOR_ID(APTX_AD_VENDOR_ID, APTX_AD_CODEC_ID), { "aptX-AD", "apt-X-AD" } },
	{ A2DP_CODEC_VENDOR_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID), { "aptX-HD", "apt-X-HD" } },
	{ A2DP_CODEC_VENDOR_ID(APTX_LL_VENDOR_ID, APTX_LL_CODEC_ID), { "aptX-LL", "apt-X-LL" } },
	{ A2DP_CODEC_VENDOR_ID(APTX_TWS_VENDOR_ID, APTX_TWS_CODEC_ID), { "aptX-TWS", "apt-X-TWS" } },
	{ A2DP_CODEC_VENDOR_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID), { "FastStream", "FS" } },
	{ A2DP_CODEC_VENDOR_ID(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID), { "LC3plus" } },
	{ A2DP_CODEC_VENDOR_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID), { "LDAC" } },
	{ A2DP_CODEC_VENDOR_ID(LHDC_V1_VENDOR_ID, LHDC_V1_CODEC_ID), { "LHDC-v1" } },
	{ A2DP_CODEC_VENDOR_ID(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID), { "LHDC-v2" } },
	{ A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID), { "LHDC-v3", "LHDC-v4", "LLAC" } },
	{ A2DP_CODEC_VENDOR_ID(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID), { "LHDC-v5" } },
	{ A2DP_CODEC_VENDOR_ID(LHDC_LL_VENDOR_ID, LHDC_LL_CODEC_ID), { "LHDC-LL"} },
	{ A2DP_CODEC_VENDOR_ID(OPUS_VENDOR_ID, OPUS_CODEC_ID), { "Opus"} },
	{ A2DP_CODEC_VENDOR_ID(OPUS_PW_VENDOR_ID, OPUS_PW_CODEC_ID), { "Opus-PW"} },
	{ A2DP_CODEC_VENDOR_ID(SAMSUNG_HD_VENDOR_ID, SAMSUNG_HD_CODEC_ID), { "samsung-HD" } },
	{ A2DP_CODEC_VENDOR_ID(SAMSUNG_SC_VENDOR_ID, SAMSUNG_SC_CODEC_ID), { "samsung-SC" } },
};

/**
 * Get BlueALSA A2DP codec ID from string representation.
 *
 * @param alias Alias of an A2DP audio codec name.
 * @return BlueALSA A2DP codec ID or 0xFFFFFFFF if there was no match. */
uint32_t a2dp_codecs_codec_id_from_string(const char *alias) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		for (size_t n = 0; n < ARRAYSIZE(codecs[i].aliases); n++)
			if (codecs[i].aliases[n] != NULL &&
					strcasecmp(codecs[i].aliases[n], alias) == 0)
				return codecs[i].codec_id;
	return 0xFFFFFFFF;
}

/**
 * Get BlueALSA A2DP codec ID from vendor codec information.
 *
 * @param info A2DP vendor codec capabilities.
 * @return BlueALSA A2DP codec ID. */
uint32_t a2dp_codecs_vendor_codec_id(const a2dp_vendor_info_t *info) {
	const uint32_t vendor_id = A2DP_VENDOR_INFO_GET_VENDOR_ID(*info);
	const uint16_t codec_id = A2DP_VENDOR_INFO_GET_CODEC_ID(*info);
	return A2DP_CODEC_VENDOR_ID(vendor_id, codec_id);
}

/**
 * Convert BlueALSA A2DP codec ID into a human-readable string.
 *
 * @param codec BlueALSA A2DP audio codec ID.
 * @return Human-readable string or NULL for unknown codec. */
const char *a2dp_codecs_codec_id_to_string(uint32_t codec_id) {
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
