/*
 * BlueALSA - bluetooth-hfp.c
 * SPDX-FileCopyrightText: 2017-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bluetooth-hfp.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "defs.h"

static const struct {
	uint8_t codec;
	const char * aliases[1];
} codecs[] = {
	{ HFP_CODEC_CVSD, { "CVSD" } },
	{ HFP_CODEC_MSBC, { "mSBC" } },
	{ HFP_CODEC_LC3_SWB, { "LC3-SWB" } },
};

ssize_t hfp_ag_features_to_strings(uint32_t features, const char ** out, size_t size) {

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

ssize_t hfp_hf_features_to_strings(uint32_t features, const char ** out, size_t size) {

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

uint8_t hfp_codec_from_string(const char * alias) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		for (size_t n = 0; n < ARRAYSIZE(codecs[i].aliases); n++)
			if (codecs[i].aliases[n] != NULL &&
					strcasecmp(codecs[i].aliases[n], alias) == 0)
				return codecs[i].codec;
	return HFP_CODEC_UNDEFINED;
}

const char * hfp_codec_to_string(uint8_t codec) {
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (codecs[i].codec == codec)
			return codecs[i].aliases[0];
	return NULL;
}
