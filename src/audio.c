/*
 * BlueALSA - audio.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "audio.h"

#include <endian.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <glib.h>

#include "shared/defs.h"

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
void audio_interleave_s16_2le(const int16_t *ch1, const int16_t *ch2,
		size_t frames, unsigned int channels, int16_t *dest) {
	const int16_t *src[] = { ch1, ch2 };
	g_assert_cmpint(channels, <=, ARRAYSIZE(src));
	for (size_t f = 0; f < frames; f++)
		for (unsigned int c = 0; c < channels; c++)
			*dest++ = src[c][f];
}

/**
 * Join channels into interleaved S32 PCM signal. */
void audio_interleave_s32_4le(const int32_t *ch1, const int32_t *ch2,
		size_t frames, unsigned int channels, int32_t *dest) {
	const int32_t *src[] = { ch1, ch2 };
	g_assert_cmpint(channels, <=, ARRAYSIZE(src));
	for (size_t f = 0; f < frames; f++)
		for (unsigned int c = 0; c < channels; c++)
			*dest++ = src[c][f];
}

/**
 * Split interleaved S16 PCM signal into channels. */
void audio_deinterleave_s16_2le(const int16_t *src, size_t frames,
		unsigned int channels, int16_t *dest1, int16_t *dest2) {
	int16_t *dest[] = { dest1, dest2 };
	g_assert_cmpint(channels, <=, ARRAYSIZE(dest));
	for (size_t f = 0; f < frames; f++)
		for (unsigned int c = 0; c < channels; c++)
			dest[c][f] = *src++;
}

/**
 * Split interleaved S32 PCM signal into channels. */
void audio_deinterleave_s32_4le(const int32_t *src, size_t frames,
		unsigned int channels, int32_t *dest1, int32_t *dest2) {
	int32_t *dest[] = { dest1, dest2 };
	g_assert_cmpint(channels, <=, ARRAYSIZE(dest));
	for (size_t f = 0; f < frames; f++)
		for (unsigned int c = 0; c < channels; c++)
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
 * @param frames The number of PCM frames in the buffer.
 * @param channels The number of channels in the buffer.
 * @param ch1 The scaling factor for 1st channel.
 * @param ch1 The scaling factor for 2nd channel. */
void audio_scale_s16_2le(int16_t *buffer, size_t frames,
		unsigned int channels, double ch1, double ch2) {
	audio_silence_s16_2le(buffer, frames, channels, ch1 == 0, ch2 == 0);
	switch (channels) {
	case 1:
		if (ch1 != 0 && ch1 != 1)
			while (frames--) {
				int16_t s1 = (int16_t)le16toh(buffer[frames]) * ch1;
				buffer[frames] = htole16(s1);
			}
		break;
	case 2:
		if ((ch1 != 0 && ch1 != 1) || (ch2 != 0 && ch2 != 1))
			while (frames--) {
				int16_t s1 = (int16_t)le16toh(buffer[2 * frames]) * ch1;
				buffer[2 * frames] = htole16(s1);
				int16_t s2 = (int16_t)le16toh(buffer[2 * frames + 1]) * ch2;
				buffer[2 * frames + 1] = htole16(s2);
			}
		break;
	default:
		g_assert_not_reached();
	}
}

/**
 * Scale S32_4LE PCM signal. */
void audio_scale_s32_4le(int32_t *buffer, size_t frames,
		unsigned int channels, double ch1, double ch2) {
	audio_silence_s32_4le(buffer, frames, channels, ch1 == 0, ch2 == 0);
	switch (channels) {
	case 1:
		if (ch1 != 0 && ch1 != 1)
			while (frames--) {
				int32_t s1 = (int32_t)le32toh(buffer[frames]) * ch1;
				buffer[frames] = htole32(s1);
			}
		break;
	case 2:
		if ((ch1 != 0 && ch1 != 1) || (ch2 != 0 && ch2 != 1))
			while (frames--) {
				int32_t s1 = (int32_t)le32toh(buffer[2 * frames]) * ch1;
				buffer[2 * frames] = htole32(s1);
				int32_t s2 = (int32_t)le32toh(buffer[2 * frames + 1]) * ch2;
				buffer[2 * frames + 1] = htole32(s2);
			}
		break;
	default:
		g_assert_not_reached();
	}
}

/**
 * Silence S16_2LE PCM signal. */
void audio_silence_s16_2le(int16_t *buffer, size_t frames,
		unsigned int channels, bool ch1, bool ch2) {
	switch (channels) {
	case 1:
		if (ch1)
			memset(buffer, 0, frames * sizeof(*buffer));
		break;
	case 2:
		if (ch1 || ch2) {
			uint32_t mask = be32toh((ch1 ? 0 : 0xFFFF0000) | (ch2 ? 0 : 0xFFFF));
			while (frames--)
				((uint32_t *)buffer)[frames] &= mask;
		}
		break;
	default:
		g_assert_not_reached();
	}
}

/**
 * Silence S32_4LE PCM signal. */
void audio_silence_s32_4le(int32_t *buffer, size_t frames,
		unsigned int channels, bool ch1, bool ch2) {
	switch (channels) {
	case 1:
		if (ch1)
			memset(buffer, 0, frames * sizeof(*buffer));
		break;
	case 2:
		if (ch1 || ch2) {
			uint32_t mask_ch1 = ch1 ? 0 : 0xFFFFFFFF;
			uint32_t mask_ch2 = ch2 ? 0 : 0xFFFFFFFF;
			for (size_t i = 0; i < frames * 2; i += 2) {
				buffer[i + 0] &= mask_ch1;
				buffer[i + 1] &= mask_ch2;
			}
		}
		break;
	default:
		g_assert_not_reached();
	}
}
