/*
 * BlueALSA - audio.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "audio.h"

#include <endian.h>
#include <math.h>

/**
 * Convert audio volume change in dB to loudness.
 *
 * @param value The audio volume change in dB.
 * @return This function returns the loudness value which
 *   corresponds to the given audio volume change in dB. */
double audio_decibel_to_loudness(double value) {
	return pow(2, value / 10);
}

/**
 * Convert loudness to audio volume change in dB.
 *
 * @param value The volume loudness value.
 * @return This function returns the audio volume change
 *   in dB which corresponds to the given loudness value. */
double audio_loudness_to_decibel(double value) {
	return 10 * log2(value);
}

/**
 * Join channels into interleaved S16 PCM signal. */
void audio_interleave_s16_2le(int16_t * restrict dest,
		const int16_t * restrict * restrict src, unsigned int channels, size_t frames) {
	for (size_t f = 0; f < frames; f++)
		for (size_t c = 0; c < channels; c++)
			*dest++ = src[c][f];
}

/**
 * Join channels into interleaved S32 PCM signal. */
void audio_interleave_s32_4le(int32_t * restrict dest,
		const int32_t * restrict * restrict src, unsigned int channels, size_t frames) {
	for (size_t f = 0; f < frames; f++)
		for (size_t c = 0; c < channels; c++)
			*dest++ = src[c][f];
}

/**
 * Split interleaved S16 PCM signal into channels. */
void audio_deinterleave_s16_2le(int16_t * restrict * restrict dest,
		const int16_t * restrict src, unsigned int channels, size_t frames) {
	for (size_t f = 0; f < frames; f++)
		for (size_t c = 0; c < channels; c++)
			dest[c][f] = *src++;
}

/**
 * Split interleaved S32 PCM signal into channels. */
void audio_deinterleave_s32_4le(int32_t * restrict * restrict dest,
		const int32_t * restrict src, unsigned int channels, size_t frames) {
	for (size_t f = 0; f < frames; f++)
		for (size_t c = 0; c < channels; c++)
			dest[c][f] = *src++;
}

/**
 * Scale S16_2LE PCM signal.
 *
 * Neutral value for scaling factor is 1.0. It is possible to increase
 * signal gain by using scaling factor values greater than 1, however,
 * clipping will most certainly occur.
 *
 * @param buffer Address to the buffer where the PCM signal is stored.
 * @param scale The scaling factor per channel for the PCM signal.
 * @param channels The number of channels in the buffer.
 * @param frames The number of PCM frames in the buffer. */
void audio_scale_s16_2le(int16_t * restrict buffer,
		const double * restrict scale, unsigned int channels, size_t frames) {
	for (size_t i = 0; frames; frames--)
		for (size_t c = 0; c < channels; c++, i++)
			buffer[i] = htole16((int16_t)((int16_t)le16toh(buffer[i]) * scale[c]));
}

/**
 * Scale S32_4LE PCM signal. */
void audio_scale_s32_4le(int32_t * restrict buffer,
		const double * restrict scale, unsigned int channels, size_t frames) {
	for (size_t i = 0; frames; frames--)
		for (size_t c = 0; c < channels; c++, i++)
			buffer[i] = htole32((int32_t)((int32_t)le32toh(buffer[i]) * scale[c]));
}
