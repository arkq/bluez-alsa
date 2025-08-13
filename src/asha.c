/*
 * BlueALSA - asha.c
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "asha.h"

#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include <glib.h>

#include "asha-g722.h"
#include "ba-transport-pcm.h"

/**
 * Get ASHA codec ID from string representation.
 *
 * @param alias Alias of ASHA audio codec name.
 * @return ASHA audio codec ID or ASHA_CODEC_UNDEFINED in case of no match. */
uint8_t asha_codec_id_from_string(const char * alias) {
	if (strcasecmp(alias, "G722") == 0)
		return ASHA_CODEC_G722;
	return ASHA_CODEC_UNDEFINED;
}

/**
 * Convert ASHA codec ID into a human-readable string.
 *
 * @param codec ASHA audio codec ID.
 * @return Human-readable string or NULL for unknown codec. */
const char * asha_codec_id_to_string(uint8_t codec_id) {
	if (codec_id == ASHA_CODEC_G722)
		return "G722";
	return NULL;
}

int asha_transport_start(struct ba_transport * t) {
	if (t->profile == BA_TRANSPORT_PROFILE_ASHA_SOURCE)
		return ba_transport_pcm_start(&t->media.pcm, asha_g722_enc_thread, "ba-asha-g722");
	if (t->profile == BA_TRANSPORT_PROFILE_ASHA_SINK)
		return ba_transport_pcm_start(&t->media.pcm, asha_g722_dec_thread, "ba-asha-g722");
	g_assert_not_reached();
	return -1;
}
