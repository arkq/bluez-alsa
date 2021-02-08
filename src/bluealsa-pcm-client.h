/*
 * BlueALSA - bluealsa-pcm-client.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_PCM_CLIENT_H
#define BLUEALSA_PCM_CLIENT_H

#include <stdint.h>
#include <stdbool.h>


struct bluealsa_pcm_client;
struct bluealsa_pcm_multi;


enum bluealsa_pcm_client_state {
	BLUEALSA_PCM_CLIENT_STATE_IDLE,
	BLUEALSA_PCM_CLIENT_STATE_RUNNING,
	BLUEALSA_PCM_CLIENT_STATE_PAUSED,
	BLUEALSA_PCM_CLIENT_STATE_DRAINING,
	BLUEALSA_PCM_CLIENT_STATE_FINISHED,
};

enum bluealsa_pcm_client_event_type {
	BLUEALSA_EVENT_TYPE_PCM,
	BLUEALSA_EVENT_TYPE_CONTROL,
	BLUEALSA_EVENT_TYPE_DRAIN,
};

struct bluealsa_pcm_client_event {
	enum bluealsa_pcm_client_event_type type;
	struct bluealsa_pcm_client *client;
};

struct bluealsa_pcm_client {
	struct bluealsa_pcm_multi *multi;
	int pcm_fd;
	int control_fd;
	int drain_timer_fd;
	struct bluealsa_pcm_client_event pcm_event;
	struct bluealsa_pcm_client_event control_event;
	struct bluealsa_pcm_client_event drain_event;
	enum bluealsa_pcm_client_state state;
	uint8_t *buffer;
	size_t buffer_size;
	size_t in_offset;
	ssize_t out_offset;
	bool watch;
#if DEBUG
	size_t id;
#endif
};



struct bluealsa_pcm_client *bluealsa_pcm_client_new(
                 struct bluealsa_pcm_multi *multi, int pcm_fd, int control_fd);

bool bluealsa_pcm_client_init(struct bluealsa_pcm_client *client);

void bluealsa_pcm_client_free(struct bluealsa_pcm_client *client);

void bluealsa_pcm_client_handle_event(struct bluealsa_pcm_client_event *event);
void bluealsa_pcm_client_handle_close_event(
                                      struct bluealsa_pcm_client_event *event);
void bluealsa_pcm_client_deliver(struct bluealsa_pcm_client *client);
void bluealsa_pcm_client_fetch(struct bluealsa_pcm_client *client);
void bluealsa_pcm_client_write(struct bluealsa_pcm_client *client);

#endif /* BLUEALSA_PCM_CLIENT_H */
