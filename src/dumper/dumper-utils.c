/*
 * BlueALSA - dumper-utils.c
 * SPDX-FileCopyrightText: 2021-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "dumper.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>
#include <stdio.h>

#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "shared/bluetooth-a2dp.h"
#include "shared/bluetooth-asha.h"
#include "shared/bluetooth-hfp.h"

const char * ba_transport_to_string(const struct ba_transport * t) {

	const unsigned int mask = ba_dumper_profile_to_mask(t->profile);
	const char * profile = ba_dumper_profile_mask_to_string(mask);
	const uint32_t codec_id = ba_transport_get_codec(t);
	const char * codec = NULL;

	switch (mask) {
	case BA_TRANSPORT_PROFILE_MASK_A2DP:
		codec = a2dp_codec_to_string(codec_id);
		break;
#if ENABLE_ASHA
	case BA_TRANSPORT_PROFILE_MASK_ASHA:
		codec = asha_codec_to_string(codec_id);
		break;
#endif
	case BA_TRANSPORT_PROFILE_MASK_HFP:
	case BA_TRANSPORT_PROFILE_MASK_HSP:
		codec = hfp_codec_to_string(codec_id);
		break;
	default:
		/* For profiles without codec support, just return the profile name. */
		return profile;
	}

	char fallback[16];
	if (codec == NULL) {
		snprintf(fallback, sizeof(fallback), "%08x", codec_id);
		codec = fallback;
	}

	static char buffer[64];
	snprintf(buffer, sizeof(buffer), "%s-%s", profile, codec);
	return buffer;
}

const char * ba_transport_pcm_to_string(const struct ba_transport_pcm * t_pcm) {
	static char buffer[64];
	snprintf(buffer, sizeof(buffer), "s%u-%u-%uc",
			BA_TRANSPORT_PCM_FORMAT_WIDTH(t_pcm->format), t_pcm->rate, t_pcm->channels);
	return buffer;
}
