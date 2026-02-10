/*
 * BlueALSA - dumper.c
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "dumper.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/types.h>

#include "a2dp.h"
#include "ba-transport.h"
#include "shared/bluetooth-a2dp.h"
#include "shared/bluetooth-asha.h"
#include "shared/bluetooth-hfp.h"
#include "shared/defs.h"
#include "shared/hex.h"

static const struct {
	unsigned int mask;
	const char * name;
} profiles[] = {
	{ BA_TRANSPORT_PROFILE_MASK_A2DP, "A2DP" },
#if ENABLE_ASHA
	{ BA_TRANSPORT_PROFILE_MASK_ASHA, "ASHA" },
#endif
	{ BA_TRANSPORT_PROFILE_MASK_HFP, "HFP" },
	{ BA_TRANSPORT_PROFILE_MASK_HSP, "HSP" },
#if ENABLE_MIDI
	{ BA_TRANSPORT_PROFILE_MASK_MIDI, "MIDI" },
#endif
};

unsigned int ba_dumper_profile_to_mask(enum ba_transport_profile profile) {
	for (size_t i = 0; i < ARRAYSIZE(profiles); i++)
		if (profile & profiles[i].mask)
			return profiles[i].mask;
	return 0;
}

unsigned int ba_dumper_profile_mask_from_string(const char * name) {
	for (size_t i = 0; i < ARRAYSIZE(profiles); i++)
		if (strcasecmp(name, profiles[i].name) == 0)
			return profiles[i].mask;
	return 0;
}

const char * ba_dumper_profile_mask_to_string(unsigned int mask) {
	for (size_t i = 0; i < ARRAYSIZE(profiles); i++)
		if (mask == profiles[i].mask)
			return profiles[i].name;
	return "UNKNOWN";
}

ssize_t ba_dumper_read_header(
		FILE * stream,
		uint32_t * profile_mask,
		uint32_t * codec_id,
		void * configuration,
		size_t * configuration_size) {

	char buffer[1024];
	if (fgets(buffer, sizeof(buffer), stream) == NULL)
		return -1;

	const size_t len = strlen(buffer);

	size_t i = len;
	/* Trim trailing newline character. */
	while (i > 0 && isspace(buffer[--i]))
		buffer[i] = '\0';

	uint32_t _profile_mask = 0;
	uint32_t _codec_id = 0;
	size_t _configuration_size = 0;

	char * profile = NULL;
	char * codec = NULL;
	char * hex = NULL;
	int n = sscanf(buffer, "%m[^:]:%m[^:]:%ms", &profile, &codec, &hex);

	if (n >= 1)
		_profile_mask = ba_dumper_profile_mask_from_string(profile);

	if (n >= 2)
		switch (_profile_mask) {
		case BA_TRANSPORT_PROFILE_MASK_A2DP:
			_codec_id = a2dp_codec_from_string(codec);
			break;
#if ENABLE_ASHA
		case BA_TRANSPORT_PROFILE_MASK_ASHA:
			_codec_id = asha_codec_from_string(codec);
			break;
#endif
		case BA_TRANSPORT_PROFILE_MASK_HFP:
		case BA_TRANSPORT_PROFILE_MASK_HSP:
			_codec_id = hfp_codec_from_string(codec);
			break;
		}

	if (n >= 3) {
		size_t hex_len = strlen(hex);
		hex_len = MIN(*configuration_size * 2, hex_len);
		_configuration_size = hex2bin(hex, configuration, hex_len);
	}

	free(profile);
	free(codec);
	free(hex);

	*profile_mask = _profile_mask;
	*codec_id = _codec_id;
	*configuration_size = _configuration_size;

	return len;
}

ssize_t ba_dumper_write_header(
		FILE * stream,
		const struct ba_transport * t) {

	const uint32_t codec_id = ba_transport_get_codec(t);
	size_t len = 0;
	int n;

	const unsigned int mask = ba_dumper_profile_to_mask(t->profile);
	if ((n = fprintf(stream, "%s", ba_dumper_profile_mask_to_string(mask))) < 0)
		return -1;
	len += n;

	switch (mask) {
	case BA_TRANSPORT_PROFILE_MASK_A2DP:
		if ((n = fprintf(stream, ":%s", a2dp_codec_to_string(codec_id))) < 0)
			return -1;
		len += n;
		char hex[sizeof(t->media.a2dp.configuration) * 2 + 1];
		bin2hex(&t->media.a2dp.configuration, hex, t->media.a2dp.sep->config.caps_size);
		if ((n = fprintf(stream, ":%s", hex)) < 0)
			return -1;
		len += n;
		break;
#if ENABLE_ASHA
	case BA_TRANSPORT_PROFILE_MASK_ASHA:
		if ((n = fprintf(stream, ":%s", asha_codec_to_string(codec_id))) < 0)
			return -1;
		len += n;
		break;
#endif
	case BA_TRANSPORT_PROFILE_MASK_HFP:
	case BA_TRANSPORT_PROFILE_MASK_HSP:
		if ((n = fprintf(stream, ":%s", hfp_codec_to_string(codec_id))) < 0)
			return -1;
		len += n;
		break;
	}

	if ((n = fprintf(stream, "\n")) < 0)
		return -1;
	len += n;

	return len;
}

ssize_t ba_dumper_read(
		FILE * stream,
		void * data,
		size_t size) {

	char buffer[4096];
	if (fgets(buffer, sizeof(buffer), stream) == NULL)
		return -1;

	size_t n;
	char * hex = NULL;
	if ((n = strtoul(buffer, &hex, 16)) == 0)
		return -1;

	if (n > size)
		return -1;

	while (isspace(*hex))
		hex++;

	hex2bin(hex, data, n * 2);
	return n;
}

ssize_t ba_dumper_write(
		FILE * stream,
		const void * data,
		size_t size) {
	char buffer[size * 2 + 1];
	bin2hex(data, buffer, size);
	return fprintf(stream, "%04zX %s\n", size, buffer);
}
