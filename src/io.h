/*
 * BlueALSA - io.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_IO_H_
#define BLUEALSA_IO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stddef.h>

#include "ba-transport.h"
#include "shared/rt.h"

/**
 * Callback function for thread signal filtering. */
typedef enum ba_transport_thread_signal io_poll_signal_filter(
		enum ba_transport_thread_signal signal,
		void *userdata);

/**
 * Data associated with IO polling.
 *
 * Note:
 * The timeout field shall be initialized to -1. */
struct io_poll {
	struct {
		/* signal filtering callback */
		io_poll_signal_filter *filter;
		void *userdata;
	} signal;
	/* transfer bit rate synchronization */
	struct asrsync asrs;
	/* keep-alive and sync timeout */
	int timeout;
};

ssize_t io_bt_read(
		struct ba_transport_thread *th,
		void *buffer,
		size_t count);

ssize_t io_bt_write(
		struct ba_transport_thread *th,
		const void *buffer,
		size_t count);

void io_pcm_scale(
		const struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples);

ssize_t io_pcm_flush(
		struct ba_transport_pcm *pcm);

ssize_t io_pcm_read(
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples);

ssize_t io_pcm_write(
		struct ba_transport_pcm *pcm,
		const void *buffer,
		size_t samples);

ssize_t io_poll_and_read_bt(
		struct io_poll *io,
		struct ba_transport_thread *th,
		void *buffer,
		size_t count);

ssize_t io_poll_and_read_pcm(
		struct io_poll *io,
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples);

#endif
