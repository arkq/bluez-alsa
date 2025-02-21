/*
 * BlueALSA - io.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
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

#include <stdbool.h>
#include <sys/types.h>

#include "ba-transport-pcm.h"
#include "shared/ffb.h"
#include "shared/rt.h"

/**
 * Data associated with IO polling.
 *
 * Note:
 * The timeout field shall be initialized to -1. */
struct io_poll {
	/* transfer bit rate synchronization */
	struct asrsync asrs;
	/* transfer has been initiated */
	bool initiated;
	/* true when we have received data after the last drain */
	bool tainted;
	/* true when drain is in progress */
	bool draining;
	/* keep-alive and drain timeout */
	int timeout;
};

ssize_t io_bt_read(
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t count);

ssize_t io_bt_write(
		struct ba_transport_pcm *pcm,
		const void *buffer,
		size_t count);

void io_pcm_scale(
		struct ba_transport_pcm *pcm,
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
		struct ba_transport_pcm *pcm,
		ffb_t *buffer);

ssize_t io_poll_and_read_pcm(
		struct io_poll *io,
		struct ba_transport_pcm *pcm,
		ffb_t *buffer);

#endif
