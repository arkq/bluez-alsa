/*
 * BlueALSA - bluetooth-asha.c
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bluetooth-asha.h"

#include <stdint.h>
#include <strings.h>

uint8_t asha_codec_from_string(const char * alias) {
	if (strcasecmp(alias, "G722") == 0)
		return ASHA_CODEC_G722;
	return ASHA_CODEC_UNDEFINED;
}

const char * asha_codec_to_string(uint8_t codec) {
	if (codec == ASHA_CODEC_G722)
		return "G722";
	return NULL;
}
