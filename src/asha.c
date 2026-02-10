/*
 * BlueALSA - asha.c
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "asha.h"

#include <glib.h>

#include "asha-g722.h"
#include "ba-transport-pcm.h"

int asha_transport_start(struct ba_transport * t) {
	if (t->profile == BA_TRANSPORT_PROFILE_ASHA_SOURCE)
		return ba_transport_pcm_start(&t->media.pcm, asha_g722_enc_thread, "ba-asha-g722");
	if (t->profile == BA_TRANSPORT_PROFILE_ASHA_SINK)
		return ba_transport_pcm_start(&t->media.pcm, asha_g722_dec_thread, "ba-asha-g722");
	g_assert_not_reached();
	return -1;
}
