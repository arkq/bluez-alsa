/*
 * BlueALSA - audio.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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

#include <stddef.h>
#include <stdint.h>

double audio_decibel_to_loudness(double value);
double audio_loudness_to_decibel(double value);

void audio_interleave_s16_2le(int16_t * restrict dest,
		const int16_t * restrict * restrict src, unsigned int channels, size_t frames);
void audio_interleave_s32_4le(int32_t * restrict dest,
		const int32_t * restrict * restrict src, unsigned int channels, size_t frames);
#define audio_interleave_s24_4le audio_interleave_s32_4le

void audio_deinterleave_s16_2le(int16_t * restrict * restrict dest,
		const int16_t * restrict src, unsigned int channels, size_t frames);
void audio_deinterleave_s32_4le(int32_t * restrict * restrict dest,
		const int32_t * restrict src, unsigned int channels, size_t frames);
#define audio_deinterleave_s24_4le audio_deinterleave_s32_4le

void audio_scale_s16_2le(int16_t * restrict buffer, const double * restrict scale,
		unsigned int channels, size_t frames);
void audio_scale_s32_4le(int32_t * restrict buffer, const double * restrict scale,
		unsigned int channels, size_t frames);
#define audio_scale_s24_4le audio_scale_s32_4le

#endif
