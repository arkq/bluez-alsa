/*
 * BlueALSA - sine.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_SHARED_SINE_H_
#define BLUEALSA_SHARED_SINE_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stddef.h>
#include <stdint.h>

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
size_t snd_pcm_sine_s16_2le(
		int16_t * restrict dest,
		unsigned int channels,
		size_t frames,
		float f,
		size_t x);

/**
 * Generate sine S32_4LE PCM signal. */
size_t snd_pcm_sine_s32_4le(
		int32_t * restrict dest,
		unsigned int channels,
		size_t frames,
		float f,
		size_t x);

/**
 * Generate sine S24_4LE PCM signal. */
size_t snd_pcm_sine_s24_4le(
		int32_t * restrict dest,
		unsigned int channels,
		size_t frames,
		float f,
		size_t x);

#endif
