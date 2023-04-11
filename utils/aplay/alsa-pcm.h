/*
 * BlueALSA - alsa-pcm.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_ALSAPCM_H_
#define BLUEALSA_APLAY_ALSAPCM_H_

#include <stdio.h>

#include <alsa/asoundlib.h>

int alsa_pcm_open(snd_pcm_t **pcm, const char *name,
		snd_pcm_format_t format, int channels, int rate,
		unsigned int *buffer_time, unsigned int *period_time,
		char **msg);

void alsa_pcm_dump(snd_pcm_t *pcm, FILE *fp);

#endif
