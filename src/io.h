/*
 * BlueALSA - io.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_IO_H_
#define BLUEALSA_IO_H_

#if HAVE_CONFIG_H
# include "config.h"
#endif

/* Casting wrapper for the brevity's sake. */
#define CANCEL_ROUTINE(f) ((void (*)(void *))(f))

void *io_thread_a2dp_sink_sbc(void *arg);
void *io_thread_a2dp_source_sbc(void *arg);
#if ENABLE_AAC
void *io_thread_a2dp_sink_aac(void *arg);
void *io_thread_a2dp_source_aac(void *arg);
#endif
#if ENABLE_APTX
void *io_thread_a2dp_source_aptx(void *arg);
#endif

void *io_thread_sco(void *arg);

#endif
