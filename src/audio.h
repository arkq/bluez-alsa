/*
 * BlueALSA - audio.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_AUDIO_H_
#define BLUEALSA_AUDIO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void audio_scale_s16le(int16_t *buffer, int channels, size_t frames, double ch1, double ch2);
void audio_silence_s16le(int16_t *buffer, int channels, size_t frames, bool ch1, bool ch2);

#endif
