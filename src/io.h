/*
 * BlueALSA - io.h
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
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

/* The number of snapshots of BT socket COUTQ bytes. */
#define IO_THREAD_COUTQ_HISTORY_SIZE 16

void *io_thread_a2dp_sink_sbc(void *arg);
void *io_thread_a2dp_source_sbc(void *arg);
#if ENABLE_AAC
void *io_thread_a2dp_sink_aac(void *arg);
void *io_thread_a2dp_source_aac(void *arg);
#endif
#if ENABLE_APTX
void *io_thread_a2dp_source_aptx(void *arg);
#endif
#if ENABLE_LDAC
void *io_thread_a2dp_source_ldac(void *arg);
#endif

void *io_thread_sco(void *arg);

#if DEBUG
void *io_thread_a2dp_sink_dump(void *arg);
#endif

#endif
