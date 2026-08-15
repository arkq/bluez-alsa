/*
 * BlueALSA - sine.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "sine.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>

size_t snd_pcm_sine_s16_2le(
		int16_t * restrict dest,
		unsigned int channels,
		size_t frames,
		float f,
		size_t x) {
	for (size_t i = 0; i < frames; x++, i++)
		for (size_t c = 0; c < channels; c++)
			dest[i * channels + c] = sin(2 * M_PI * f * x + c * M_PI / 3.3) * SHRT_MAX;
	return x;
}

size_t snd_pcm_sine_s32_4le(
		int32_t * restrict dest,
		unsigned int channels,
		size_t frames,
		float f,
		size_t x) {
	for (size_t i = 0; i < frames; x++, i++)
		for (size_t c = 0; c < channels; c++)
			dest[i * channels + c] = sin(2 * M_PI * f * x + c * M_PI / 3.3) * INT_MAX;
	return x;
}

size_t snd_pcm_sine_s24_4le(
		int32_t * restrict dest,
		unsigned int channels,
		size_t frames,
		float f,
		size_t x) {
	x = snd_pcm_sine_s32_4le(dest, channels, frames, f, x);
	for (size_t i = 0; i < channels * frames; i++)
		dest[i] >>= 8;
	return x;
}
