/*
 * BlueALSA - a2dp.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_A2DP_H_
#define BLUEALSA_A2DP_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stddef.h>
#include <stdint.h>

#include "ba-transport.h"

ssize_t io_thread_read_pcm(struct ba_transport_pcm *pcm, int16_t *buffer, size_t samples);
ssize_t io_thread_read_pcm_flush(struct ba_transport_pcm *pcm);
ssize_t io_thread_write_pcm(struct ba_transport_pcm *pcm, const int16_t *buffer, size_t samples);

int a2dp_thread_create(struct ba_transport *t);

#endif
