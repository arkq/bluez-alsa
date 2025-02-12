/*
 * BlueALSA - ba-pcm-client.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BAPCMCLIENT_H_
#define BLUEALSA_BAPCMCLIENT_H_

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#include "ba-pcm-multi.h"
#include "config.h"

enum ba_pcm_client_state {
	/* client is registered, but not yet initialized */
	BA_PCM_CLIENT_STATE_INIT = 0,
	/* client is initialized, but not active */
	BA_PCM_CLIENT_STATE_IDLE,
	/* client is transferring audio frames */
	BA_PCM_CLIENT_STATE_RUNNING,
	/* client has sent PAUSE command, waiting for RESUME */
	BA_PCM_CLIENT_STATE_PAUSED,
	/* client has sent DRAIN command, processing frames remaining in the pipe */
	BA_PCM_CLIENT_STATE_DRAINING1,
	/* pipe is drained, waiting on timeout before returning to IDLE */
	BA_PCM_CLIENT_STATE_DRAINING2,
	/* client has closed pipe and/or control socket */
	BA_PCM_CLIENT_STATE_FINISHED,
};

enum ba_pcm_client_event_type {
	BA_EVENT_TYPE_PCM,
	BA_EVENT_TYPE_CONTROL,
	BA_EVENT_TYPE_DRAIN,
};

struct ba_pcm_client_event {
	enum ba_pcm_client_event_type type;
	struct ba_pcm_client *client;
};

struct ba_pcm_client {
	struct ba_pcm_multi *multi;
	/* pcm pipe endpoint */
	int pcm_fd;
	/* control socket endpoint */
	int control_fd;
	/* timer for drain completion */
	int drain_timer_fd;
	/* event structures for i/o scheduling */
	struct ba_pcm_client_event pcm_event;
	struct ba_pcm_client_event control_event;
	struct ba_pcm_client_event drain_event;
	enum ba_pcm_client_state state;
	/* pcm sample input buffer */
	uint8_t *buffer;
	size_t buffer_size;
	/* position of next free byte in pcm input buffer */
	size_t in_offset;
	/* position in mix buffer of next transfer */
	intmax_t out_offset;
	/* number of frames in mix buffer from this client yet to be drained */
	size_t drain_avail;
	/* flag indicating a Drop request has been received */
	bool drop;
	/* flag indicating the client is watching for I/O events on PCM pipe */
	bool watch;
	/* guard access to the PCM pipe endpoint */
	pthread_mutex_t mutex;
#if DEBUG
	/* when debugging use this as identifier in log messages */
	size_t id;
#endif
};

struct ba_pcm_client *ba_pcm_client_new(
					struct ba_pcm_multi *multi,
					int pcm_fd, int control_fd);

bool ba_pcm_client_init(struct ba_pcm_client *client);

void ba_pcm_client_free(struct ba_pcm_client *client);

void ba_pcm_client_handle_event(struct ba_pcm_client_event *event);
void ba_pcm_client_handle_close_event(struct ba_pcm_client_event *event);
void ba_pcm_client_deliver(struct ba_pcm_client *client);
void ba_pcm_client_fetch(struct ba_pcm_client *client);
void ba_pcm_client_write(struct ba_pcm_client *client, const void *buffer, size_t samples);
void ba_pcm_client_drain(struct ba_pcm_client *client);
void ba_pcm_client_underrun(struct ba_pcm_client *client);

#endif
