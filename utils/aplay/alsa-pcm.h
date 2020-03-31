/*
 * BlueALSA - alsa-pcm.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_APLAY_ALSAPCM_H_
#define BLUEALSA_APLAY_ALSAPCM_H_

#include <alsa/asoundlib.h>

int alsa_pcm_open(snd_pcm_t **pcm, const char *name,
		snd_pcm_format_t format, int channels, int rate,
		unsigned int *buffer_time, unsigned int *period_time,
		char **msg);

#endif
