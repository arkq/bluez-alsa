/*
 * BlueALSA - bluealsa-pcm-multi.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_PCM_MULTI_H
#define BLUEALSA_PCM_MULTI_H

#include <stdbool.h>
#include <pthread.h>
#include <glib.h>

#include "shared/ffb.h"
#include "bluealsa-mix-buffer.h"

/* Number of periods to hold in client before starting mix. */
#define BLUEALSA_MULTI_CLIENT_THRESHOLD 2


struct ba_transport_pcm;
struct bluealsa_pcm_client;

enum bluealsa_pcm_multi_state {
	BLUEALSA_PCM_MULTI_STATE_INIT = 0,
	BLUEALSA_PCM_MULTI_STATE_RUNNING,
	BLUEALSA_PCM_MULTI_STATE_PAUSED,
	BLUEALSA_PCM_MULTI_STATE_FINISHED,
};

struct bluealsa_snoop_buffer {
	const uint8_t *data;
	size_t len;
};

struct bluealsa_pcm_multi {
	struct ba_transport_pcm *pcm;
	union {
		struct bluealsa_mix_buffer playback_buffer;
		struct bluealsa_snoop_buffer capture_buffer;
	};
	size_t start_threshold;
	size_t period_bytes;
	size_t delay;
	GList *clients;
	size_t client_count;
	size_t active_count;
	enum bluealsa_pcm_multi_state state;
	int epoll_fd;
	int event_fd;
	pthread_t thread;
	pthread_mutex_t mutex;
#if DEBUG
	size_t client_no;
#endif
};

bool bluealsa_pcm_multi_enabled(struct ba_transport_pcm *pcm);

struct bluealsa_pcm_multi *bluealsa_pcm_multi_create(
                                                 struct ba_transport_pcm *pcm);

bool bluealsa_pcm_multi_init(struct bluealsa_pcm_multi *multi,
                                                      size_t transfer_samples);

void bluealsa_pcm_multi_reset(struct bluealsa_pcm_multi *multi);

void bluealsa_pcm_multi_free(struct bluealsa_pcm_multi *multi);

bool bluealsa_pcm_multi_add_client(struct bluealsa_pcm_multi *multi,
                                                   int pcm_fd, int control_fd);

ssize_t bluealsa_pcm_multi_read(struct bluealsa_pcm_multi *multi,
                                               void *buffer, size_t samples);

void bluealsa_pcm_multi_write(struct bluealsa_pcm_multi *multi,
                                                 void *buffer, size_t samples);

ssize_t bluealsa_pcm_multi_fetch(struct bluealsa_pcm_multi *multi,
                                void *buffer, size_t samples, bool *restarted);

#endif /* BLUEALSA_PCM_MULTI_H */
