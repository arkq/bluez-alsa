/*
 * sine.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Generate sine S16_2LE PCM signal.
 *
 * @param dest Address of the PCM buffer, where the data will be stored.
     This buffer has to be big enough to store channels * frames number of
     PCM samples.
 * @param channels Number of channels per PCM frame. The sine wave for each
 *   channel is phase shifted by PI/3.3.
 * @param frames The number of PCM frames to generate.
 * @param f Required sine frequency divided by the PCM sample rate.
 * @param x Sample counter.
 * @return Updated x parameter. One may use this value for a next call, in
 *   order to generate smooth sine curve. */
size_t snd_pcm_sine_s16_2le(int16_t *dest, unsigned int channels, size_t frames,
		float f, size_t x) {
	for (size_t i = 0; i < frames; x++, i++)
		for (size_t c = 0; c < channels; c++)
			dest[i * channels + c] = sin(2 * M_PI * f * x + c * M_PI / 3.3) * SHRT_MAX;
	return x;
}

/**
 * Generate sine S32_4LE PCM signal. */
size_t snd_pcm_sine_s32_4le(int32_t *dest, unsigned int channels, size_t frames,
		float f, size_t x) {
	for (size_t i = 0; i < frames; x++, i++)
		for (size_t c = 0; c < channels; c++)
			dest[i * channels + c] = sin(2 * M_PI * f * x + c * M_PI / 3.3) * INT_MAX;
	return x;
}

/**
 * Generate sine S24_4LE PCM signal. */
size_t snd_pcm_sine_s24_4le(int32_t *dest, unsigned int channels, size_t frames,
		float f, size_t x) {
	x = snd_pcm_sine_s32_4le(dest, channels, frames, f, x);
	for (size_t i = 0; i < channels * frames; i++)
		dest[i] >>= 8;
	return x;
}
