/*
 * BlueALSA - a2dp-audio.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_A2DPAUDIO_H_
#define BLUEALSA_A2DPAUDIO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stddef.h>
#include <stdint.h>

#include "ba-transport.h"

ssize_t ba_transport_pcm_flush(
		struct ba_transport_pcm *pcm);

ssize_t ba_transport_pcm_read(
		struct ba_transport_pcm *pcm,
		int16_t *buffer,
		size_t samples);

ssize_t ba_transport_pcm_write(
		struct ba_transport_pcm *pcm,
		int16_t *buffer,
		size_t samples);

int a2dp_audio_thread_create(struct ba_transport *t);

#endif
