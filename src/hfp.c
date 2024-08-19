/*
 * BlueALSA - hfp.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "hfp.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "shared/defs.h"

static const struct {
	uint8_t codec_id;
	const char *aliases[1];
} codecs[] = {
	{ HFP_CODEC_CVSD, { "CVSD" } },
	{ HFP_CODEC_MSBC, { "mSBC" } },
	{ HFP_CODEC_LC3_SWB, { "LC3-SWB" } },
};

/**
 * Convert HFP AG features into human-readable strings.
 *
 * @param features HFP AG feature mask.
 * @param out Array of strings to be filled with feature names.
 * @param size Size of the output array.
 * @return On success this function returns number of features. Otherwise, -1
 *   is returned and errno is set to indicate the error. */
ssize_t hfp_ag_features_to_strings(uint32_t features, const char **out, size_t size) {

	if (size < 12)
		return errno = ENOMEM, -1;

	size_t i = 0;

	if (features & HFP_AG_FEAT_3WC)
		out[i++] = "three-way-calling";
	if (features & HFP_AG_FEAT_ECNR)
		out[i++] = "echo-canceling-and-noise-reduction";
	if (features & HFP_AG_FEAT_VOICE)
		out[i++] = "voice-recognition";
	if (features & HFP_AG_FEAT_RING)
		out[i++] = "in-band-ring-tone";
	if (features & HFP_AG_FEAT_VTAG)
		out[i++] = "attach-voice-tag";
	if (features & HFP_AG_FEAT_REJECT)
		out[i++] = "reject-call";
	if (features & HFP_AG_FEAT_ECS)
		out[i++] = "enhanced-call-status";
	if (features & HFP_AG_FEAT_ECC)
		out[i++] = "enhanced-call-control";
	if (features & HFP_AG_FEAT_EERC)
		out[i++] = "extended-error-codecs";
	if (features & HFP_AG_FEAT_CODEC)
		out[i++] = "codec-negotiation";
	if (features & HFP_AG_FEAT_HF_IND)
		out[i++] = "hf-indicators";
	if (features & HFP_AG_FEAT_ESCO)
		out[i++] = "esco-s4-settings";

	return i;
}

/**
 * Convert HFP HF features into human-readable strings.
 *
 * @param features HFP HF feature mask.
 * @param out Array of strings to be filled with feature names.
 * @param size Size of the output array.
 * @return On success this function returns number of features. Otherwise, -1
 *   is returned and errno is set to indicate the error. */
ssize_t hfp_hf_features_to_strings(uint32_t features, const char **out, size_t size) {

	if (size < 10)
		return errno = ENOMEM, -1;

	size_t i = 0;

	if (features & HFP_HF_FEAT_ECNR)
		out[i++] = "echo-canceling-and-noise-reduction";
	if (features & HFP_HF_FEAT_3WC)
		out[i++] = "three-way-calling";
	if (features & HFP_HF_FEAT_CLI)
		out[i++] = "cli-presentation";
	if (features & HFP_HF_FEAT_VOICE)
		out[i++] = "voice-recognition";
	if (features & HFP_HF_FEAT_VOLUME)
		out[i++] = "volume-control";
	if (features & HFP_HF_FEAT_ECS)
		out[i++] = "enhanced-call-status";
	if (features & HFP_HF_FEAT_ECC)
		out[i++] = "enhanced-call-control";
	if (features & HFP_HF_FEAT_CODEC)
		out[i++] = "codec-negotiation";
	if (features & HFP_HF_FEAT_HF_IND)
		out[i++] = "hf-indicators";
	if (features & HFP_HF_FEAT_ESCO)
		out[i++] = "esco-s4-settings";

	return i;
}

/**
 * Get BlueALSA HFP codec ID from string representation.
 *
 * @param alias Alias of HFP audio codec name.
 * @return BlueALSA HFP audio codec ID or HFP_CODEC_UNDEFINED if there was no
 *   match. */
uint8_t hfp_codec_id_from_string(const char *alias) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		for (size_t n = 0; n < ARRAYSIZE(codecs[i].aliases); n++)
			if (codecs[i].aliases[n] != NULL &&
					strcasecmp(codecs[i].aliases[n], alias) == 0)
				return codecs[i].codec_id;
	return HFP_CODEC_UNDEFINED;
}

/**
 * Convert BlueALSA HFP codec ID into a human-readable string.
 *
 * @param codec BlueALSA HFP audio codec ID.
 * @return Human-readable string or NULL for unknown codec. */
const char *hfp_codec_id_to_string(uint8_t codec_id) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (codecs[i].codec_id == codec_id)
			return codecs[i].aliases[0];
	return NULL;
}
