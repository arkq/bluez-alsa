/*
 * BlueALSA - resampler.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_RESAMPLER_H_
#define BLUEALSA_APLAY_RESAMPLER_H_

#include <endian.h>
#include <alsa/asoundlib.h>
#include <stdbool.h>

#include "shared/ffb.h"

struct aplay_resampler;

enum aplay_converter {
	APLAY_CONV_SINC_BEST_QUALITY        = 0,
	APLAY_CONV_CONV_SINC_MEDIUM_QUALITY = 1,
	APLAY_CONV_SINC_FASTEST             = 2,
	APLAY_CONV_ZERO_ORDER_HOLD          = 3,
	APLAY_CONV_LINEAR                   = 4,
};

bool resampler_supports_input_format(snd_pcm_format_t format);

struct aplay_resampler *resampler_create(
			enum aplay_converter converter_type,
			unsigned int channels,
			snd_pcm_format_t in_format,
			unsigned int in_rate,
			snd_pcm_format_t out_format,
			unsigned int out_rate,
			snd_pcm_uframes_t max_frames);

void resampler_delete(struct aplay_resampler *resampler);

int resampler_process(struct aplay_resampler *resampler, ffb_t *in, ffb_t *out);

bool resampler_update_rate_ratio(
			struct aplay_resampler *resampler,
			snd_pcm_uframes_t delay);

void resampler_reset(struct aplay_resampler *resampler);
double resampler_current_rate_ratio(struct aplay_resampler *resampler);
bool resampler_ready(struct aplay_resampler *resampler);

void resampler_format_le_to_native(void *buffer, size_t len, snd_pcm_format_t format);

static inline snd_pcm_format_t resampler_preferred_format(void) {
		return SND_PCM_FORMAT_FLOAT;
}

#if __BYTE_ORDER == __BIG_ENDIAN

snd_pcm_format_t resampler_native_format(snd_pcm_format_t source_format);

#elif __BYTE_ORDER == __LITTLE_ENDIAN

static inline snd_pcm_format_t resampler_native_format(snd_pcm_format_t source_format) {
	return source_format;
}

#else
# error "Unknown byte order"
#endif

#endif
