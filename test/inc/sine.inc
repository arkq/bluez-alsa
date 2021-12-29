/*
 * sine.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
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
 * @param buffer Address of the PCM buffer, where the data will be stored.
     This buffer has to be big enough to store frames * channels number of
     PCM samples.
 * @param frames The number of PCM frames to generate.
 * @param channels Number of channels per PCM frame.
 * @param x Sampling argument of the sine function.
 * @param f Required sine frequency divided by the PCM sampling frequency.
 * @return Updated x parameter. One may use this value for a next call, in
 *   order to generate smooth sine curve. */
int snd_pcm_sine_s16_2le(int16_t *buffer, size_t frames,
		unsigned int channels, int x, float f) {
	for (size_t i = 0; i < frames; x++, i++)
		for (unsigned int c = 0; c < channels; c++)
			buffer[i * channels + c] = sin(2 * M_PI * f * x + c * M_PI / 3.3) * SHRT_MAX;
	return x;
}

/**
 * Generate sine S32_4LE PCM signal. */
int snd_pcm_sine_s32_4le(int32_t *buffer, size_t frames,
		unsigned int channels, int x, float f) {
	for (size_t i = 0; i < frames; x++, i++)
		for (unsigned int c = 0; c < channels; c++)
			buffer[i * channels + c] = sin(2 * M_PI * f * x + c * M_PI / 3.3) * INT_MAX;
	return x;
}

/**
 * Generate sine S24_4LE PCM signal. */
int snd_pcm_sine_s24_4le(int32_t *buffer, size_t frames,
		unsigned int channels, int x, float f) {
	x = snd_pcm_sine_s32_4le(buffer, frames, channels, x, f);
	for (size_t i = 0; i < frames * channels; i++)
		buffer[i] >>= 8;
	return x;
}
