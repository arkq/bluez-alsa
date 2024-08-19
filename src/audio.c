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
void audio_interleave_s16_2le(int16_t *dest, const int16_t **src,
		unsigned int channels, size_t frames) {
	for (size_t f = 0; f < frames; f++)
		for (size_t c = 0; c < channels; c++)
			*dest++ = src[c][f];
}

/**
 * Join channels into interleaved S32 PCM signal. */
void audio_interleave_s32_4le(int32_t *dest, const int32_t **src,
		unsigned int channels, size_t frames) {
	for (size_t f = 0; f < frames; f++)
		for (size_t c = 0; c < channels; c++)
			*dest++ = src[c][f];
}

/**
 * Split interleaved S16 PCM signal into channels. */
void audio_deinterleave_s16_2le(int16_t **dest, const int16_t *src,
		unsigned int channels, size_t frames) {
	for (size_t f = 0; f < frames; f++)
		for (size_t c = 0; c < channels; c++)
			dest[c][f] = *src++;
}

/**
 * Split interleaved S32 PCM signal into channels. */
void audio_deinterleave_s32_4le(int32_t **dest, const int32_t *src,
		unsigned int channels, size_t frames) {
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
void audio_scale_s16_2le(int16_t *buffer, const double *scale,
		unsigned int channels, size_t frames) {
	size_t i = 0;
	while (frames--)
		for (size_t ii = 0; ii < channels; ii++) {
			int16_t s = (int16_t)le16toh(buffer[i]) * scale[ii];
			buffer[i++] = htole16(s);
		}
}

/**
 * Scale S32_4LE PCM signal. */
void audio_scale_s32_4le(int32_t *buffer, const double *scale,
		unsigned int channels, size_t frames) {
	size_t i = 0;
	while (frames--)
		for (size_t ii = 0; ii < channels; ii++) {
			int32_t s = (int32_t)le32toh(buffer[i]) * scale[ii];
			buffer[i++] = htole32(s);
		}
}
