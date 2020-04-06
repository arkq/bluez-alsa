/*
 * BlueALSA - a2dp-codecs.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-codecs.h"

#include <errno.h>

#include "hci.h"
#include "shared/log.h"

/**
 * Get BlueALSA A2DP vendor codec.
 *
 * @param capabilities A2DP vendor codec capabilities.
 * @param size A2DP vendor codec capabilities size.
 * @return On success this function returns BlueALSA A2DP vendor codec. */
uint16_t a2dp_get_bluealsa_vendor_codec(void *capabilities, size_t size) {

	if (size < sizeof(a2dp_vendor_codec_t))
		return errno = EINVAL, 0xFFFF;

	uint32_t vendor_id = A2DP_GET_VENDOR_ID(*(a2dp_vendor_codec_t *)capabilities);
	uint16_t codec_id = A2DP_GET_CODEC_ID(*(a2dp_vendor_codec_t *)capabilities);

	switch (vendor_id) {
	case BT_COMPID_QUALCOMM_TECH_INTL:
		switch (codec_id) {
		case FASTSTREAM_CODEC_ID:
			return A2DP_CODEC_VENDOR_FASTSTREAM;
		case APTX_LL_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX_LL;
		} break;
	case BT_COMPID_APT:
		switch (codec_id) {
		case APTX_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX;
		} break;
	case BT_COMPID_QUALCOMM_TECH:
		switch (codec_id) {
		case APTX_HD_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX_HD;
		} break;
	case BT_COMPID_SONY:
		switch (codec_id) {
		case LDAC_CODEC_ID:
			return A2DP_CODEC_VENDOR_LDAC;
		} break;
	}

	hexdump("Unknown vendor codec", capabilities, size);

	errno = ENOTSUP;
	return 0xFFFF;
}
