/*
 * BlueALSA - audio.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_AUDIO_H_
#define BLUEALSA_AUDIO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

double audio_decibel_to_loudness(double value);
double audio_loudness_to_decibel(double value);

void audio_interleave_s16_2le(const int16_t *ch1, const int16_t *ch2,
		size_t frames, unsigned int channels, int16_t *dest);
void audio_interleave_s32_4le(const int32_t *ch1, const int32_t *ch2,
		size_t frames, unsigned int channels, int32_t *dest);
#define audio_interleave_s24_4le audio_interleave_s32_4le

void audio_deinterleave_s16_2le(const int16_t *src, size_t frames,
		unsigned int channels, int16_t *dest1, int16_t *dest2);
void audio_deinterleave_s32_4le(const int32_t *src, size_t frames,
		unsigned int channels, int32_t *dest1, int32_t *dest2);
#define audio_deinterleave_s24_4le audio_deinterleave_s32_4le

void audio_scale_s16_2le(int16_t *buffer, size_t frames,
		unsigned int channels, double ch1, double ch2);
void audio_scale_s32_4le(int32_t *buffer, size_t frames,
		unsigned int channels, double ch1, double ch2);
#define audio_scale_s24_4le audio_scale_s32_4le

void audio_silence_s16_2le(int16_t *buffer, size_t frames,
		unsigned int channels, bool ch1, bool ch2);
void audio_silence_s32_4le(int32_t *buffer, size_t frames,
		unsigned int channels, bool ch1, bool ch2);
#define audio_silence_s24_4le audio_silence_s32_4le

#endif
