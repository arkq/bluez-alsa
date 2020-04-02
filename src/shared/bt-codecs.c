/*
 * BlueALSA - bt-codecs.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/bt-codecs.h"

#include "a2dp-codecs.h"
#include "hfp.h"

/**
 * Convert BlueALSA A2DP codec into a human-readable string.
 *
 * @param codec BlueALSA A2DP audio codec.
 * @return Human-readable string or NULL for unknown codec. */
const char *bt_codecs_a2dp_to_string(uint16_t codec) {
	switch (codec) {
	case A2DP_CODEC_SBC:
		return "SBC";
	case A2DP_CODEC_MPEG12:
		return "MP3";
	case A2DP_CODEC_MPEG24:
		return "AAC";
	case A2DP_CODEC_ATRAC:
		return "ATRAC";
	case A2DP_CODEC_VENDOR_APTX:
		return "aptX";
	case A2DP_CODEC_VENDOR_FASTSTREAM:
		return "FastStream";
	case A2DP_CODEC_VENDOR_APTX_LL:
		return "aptX-LL";
	case A2DP_CODEC_VENDOR_APTX_HD:
		return "aptX-HD";
	case A2DP_CODEC_VENDOR_LDAC:
		return "LDAC";
	default:
		return NULL;
	}
}

/**
 * Convert HFP audio codec into a human-readable string.
 *
 * @param codec HFP audio codec.
 * @return Human-readable string or NULL for unknown codec. */
const char *bt_codecs_hfp_to_string(uint16_t codec) {
	switch (codec) {
	case HFP_CODEC_CVSD:
		return "CVSD";
	case HFP_CODEC_MSBC:
		return "mSBC";
	default:
		return NULL;
	}
}
