/*
 * BlueALSA - ba-pcm-multi.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BAPCMMULTI_H_
#define BLUEALSA_BAPCMMULTI_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "ba-pcm-mix-buffer.h"

/* Number of periods to hold in mix before starting playback. */
#define BA_MULTI_MIX_THRESHOLD 4

/* Number of periods to hold in client before starting mix. */
#define BA_MULTI_CLIENT_THRESHOLD 2


struct ba_transport;
struct ba_transport_pcm;

enum ba_pcm_multi_state {
	BA_PCM_MULTI_STATE_INIT = 0,
	BA_PCM_MULTI_STATE_RUNNING,
	BA_PCM_MULTI_STATE_PAUSED,
	BA_PCM_MULTI_STATE_FINISHED,
};

struct ba_snoop_buffer {
	const uint8_t *data;
	size_t len;
};

struct ba_pcm_multi {
	struct ba_transport_pcm *pcm;
	union {
		struct ba_mix_buffer playback_buffer;
		struct ba_snoop_buffer capture_buffer;
	};
	size_t period_bytes;
	size_t period_frames;
	GList *clients;
	/* The number of clients currently connected to this multi */
	size_t client_count;
	/* The number of clients actively transferring audio */
	size_t active_count;
	_Atomic enum ba_pcm_multi_state state;
	int epoll_fd;
	int event_fd;
	pthread_t thread;
	/* Controls access to the clients list */
	pthread_mutex_t client_mutex;
	/* Controls access to the playback mix buffer */
	pthread_mutex_t buffer_mutex;
	/* Synchronize playback buffer updates */
	pthread_cond_t cond;
	bool buffer_ready;
	bool drain;
	bool drop;
#if DEBUG
	size_t client_no;
#endif
};

bool ba_pcm_multi_enabled(const struct ba_transport_pcm *pcm);

struct ba_pcm_multi *ba_pcm_multi_create(struct ba_transport_pcm *pcm);

bool ba_pcm_multi_init(struct ba_pcm_multi *multi);

void ba_pcm_multi_reset(struct ba_pcm_multi *multi);

void ba_pcm_multi_free(struct ba_pcm_multi *multi);

bool ba_pcm_multi_add_client(struct ba_pcm_multi *multi, int pcm_fd, int control_fd);

ssize_t ba_pcm_multi_read(struct ba_pcm_multi *multi, void *buffer, size_t samples);

ssize_t ba_pcm_multi_write(struct ba_pcm_multi *multi, const void *buffer, size_t samples);

ssize_t ba_pcm_multi_fetch(struct ba_pcm_multi *multi, void *buffer, size_t samples, bool *restarted);

int ba_pcm_multi_delay_get(const struct ba_pcm_multi *multi);

#endif
