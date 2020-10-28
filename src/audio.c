/*
 * BlueALSA - audio.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
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
 * Scale S16_2LE PCM signal.
 *
 * Neutral value for scaling factor is 1.0. It is possible to increase
 * signal gain by using scaling factor values greater than 1, however,
 * clipping will most certainly occur.
 *
 * @param buffer Address to the buffer where the PCM signal is stored.
 * @param channels The number of channels in the buffer.
 * @param frames The number of PCM frames in the buffer.
 * @param ch1 The scaling factor for 1st channel.
 * @param ch1 The scaling factor for 2nd channel. */
void audio_scale_s16_2le(int16_t *buffer, int channels, size_t frames, double ch1, double ch2) {
	audio_silence_s16_2le(buffer, channels, frames, ch1 == 0, ch2 == 0);
	switch (channels) {
	case 1:
		if (ch1 != 0 && ch1 != 1)
			while (frames--)
				buffer[frames] *= ch1;
		break;
	case 2:
		if ((ch1 != 0 && ch1 != 1) || (ch2 != 0 && ch2 != 1))
			while (frames--) {
				buffer[2 * frames] *= ch1;
				buffer[2 * frames + 1] *= ch2;
			}
		break;
	default:
		g_assert_not_reached();
	}
}

/**
 * Scale S32_4LE PCM signal. */
void audio_scale_s32_4le(int32_t *buffer, int channels, size_t frames, double ch1, double ch2) {
	audio_silence_s32_4le(buffer, channels, frames, ch1 == 0, ch2 == 0);
	switch (channels) {
	case 1:
		if (ch1 != 0 && ch1 != 1)
			while (frames--)
				buffer[frames] *= ch1;
		break;
	case 2:
		if ((ch1 != 0 && ch1 != 1) || (ch2 != 0 && ch2 != 1))
			while (frames--) {
				buffer[2 * frames] *= ch1;
				buffer[2 * frames + 1] *= ch2;
			}
		break;
	default:
		g_assert_not_reached();
	}
}

/**
 * Silence S16_2LE PCM signal. */
void audio_silence_s16_2le(int16_t *buffer, int channels, size_t frames, bool ch1, bool ch2) {
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
void audio_silence_s32_4le(int32_t *buffer, int channels, size_t frames, bool ch1, bool ch2) {
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
