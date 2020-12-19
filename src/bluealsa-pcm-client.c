/*
 * BlueALSA - bluealsa-pcm-client.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "ba-transport.h"
#include "bluealsa.h"
#include "bluealsa-iface.h"
#include "bluealsa-mix-buffer.h"
#include "bluealsa-pcm-client.h"
#include "bluealsa-pcm-multi.h"
#include "shared/log.h"

/* How long to wait for drain to complete, in nanosec */
#define BLUEALSA_PCM_CLIENT_DRAIN_NS 400000000

static bool bluealsa_pcm_client_is_playback(struct bluealsa_pcm_client *client) {
	return client->multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
}

static bool bluealsa_pcm_client_is_capture(struct bluealsa_pcm_client *client) {
	return client->multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE;
}

/**
 * Perform side-effects associated with a state change. */
static void bluealsa_pcm_client_set_state(struct bluealsa_pcm_client *client,
                                    enum bluealsa_pcm_client_state new_state) {
	if (new_state == client->state)
		return;

	switch (new_state) {
		case BLUEALSA_PCM_CLIENT_STATE_IDLE:
		case BLUEALSA_PCM_CLIENT_STATE_PAUSED:
		case BLUEALSA_PCM_CLIENT_STATE_FINISHED:
			if (client->state == BLUEALSA_PCM_CLIENT_STATE_RUNNING ||
			    client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING)
				client->multi->active_count--;
			break;
		case BLUEALSA_PCM_CLIENT_STATE_RUNNING:
			if (client->state == BLUEALSA_PCM_CLIENT_STATE_IDLE ||
			    client->state == BLUEALSA_PCM_CLIENT_STATE_PAUSED)
				client->multi->active_count++;
			else if (client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING)
				return;
			break;
		case BLUEALSA_PCM_CLIENT_STATE_DRAINING:
			break;
	}
	client->state = new_state;
}

/**
 * Clean up resources associated with a client PCM connection. */
static void bluealsa_pcm_client_close_pcm(struct bluealsa_pcm_client *client) {
	if (client->pcm_fd != -1) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
		client->watch = false;
		close(client->pcm_fd);
		client->pcm_fd = -1;
	}
}

/**
 * Clean up resources associated with a client control connection. */
static void bluealsa_pcm_client_close_control(
                                          struct bluealsa_pcm_client *client) {
	if (client->control_fd != -1) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL,
                                                     client->control_fd, NULL);
		close(client->control_fd);
		client->control_fd = -1;
	}
}

/**
 * Start/stop watching for PCM i/o events. */
static void bluealsa_pcm_client_watch_pcm(
                            struct bluealsa_pcm_client *client, bool enabled) {
	if (client->watch == enabled)
		return;

	uint32_t type = bluealsa_pcm_client_is_playback(client) ? EPOLLIN : EPOLLOUT;
	struct epoll_event event = {
		.events = enabled ? type : 0,
		.data.ptr = &client->pcm_event,
	};
	epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_MOD, client->pcm_fd, &event);
	client->watch = enabled;
}

/**
 * Start/stop watching for drain timer expiry event. */
static void bluealsa_pcm_client_watch_drain(
                            struct bluealsa_pcm_client *client, bool enabled) {
	struct itimerspec timeout = {
		.it_interval = { 0 },
		.it_value = {
			.tv_sec = 0,
			.tv_nsec = enabled ? BLUEALSA_PCM_CLIENT_DRAIN_NS : 0,
		},
	};
	timerfd_settime(client->drain_timer_fd, 0, &timeout, NULL);
}

/**
 * Read bytes from FIFO. */
static ssize_t bluealsa_pcm_client_read(struct bluealsa_pcm_client *client) {

	size_t space = client->buffer_size - client->in_offset;
	if (space == 0)
		return 0;

	uint8_t *buf = client->buffer + client->in_offset;

	ssize_t bytes;
	while ((bytes = read(client->pcm_fd, buf, space)) == -1 && errno == EINTR)
		continue;

	/* FIFO may be empty if client has sent DROP. */
	if (bytes == -1 && errno == EAGAIN)
		bytes = 0;

	/* pipe closed by remote end */
	if (bytes == 0)
		return -1;

	if (bytes > 0)
		client->in_offset += bytes;

	return bytes;
}

/**
 * Write byte array to FIFO.
 *
 * @return 0  on success.
 *        >0  if the call would block. The returned value is the number of
 *            bytes still to be written. Note that this may include a partial
 *            frame if count > PIPE_BUF (see man(7) pipe; on Linux PIPE_BUF is
 *            4096 bytes).
 *        -1  on failure. */
static ssize_t bluealsa_pcm_client_write_bytes(
		struct bluealsa_pcm_client *client,
		const uint8_t *data, size_t count) {

	ssize_t ret;

	do {
		if ((ret = write(client->pcm_fd, data, count)) == -1)
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
				return count;
			default:
				return ret;
			}
		data += ret;
		count -= ret;
	} while (count != 0);

	return count;
}

/**
 * Write samples to FIFO.
 *
 * Writes as many bytes as possible without blocking.
 * Sets a watch if partial write results from pipe being full.
 * Clears the watch if client buffer is emptied. */
void bluealsa_pcm_client_write(struct bluealsa_pcm_client *client) {

	size_t start = client->out_offset;
	size_t end = client->in_offset;
	size_t ret = 0;
	if (end < start) {
		/* data has wrapped. We need 2 writes - first to the end of the
		 * buffer. */
		ret = bluealsa_pcm_client_write_bytes(client,
                          client->buffer + start, client->buffer_size - start);
		start = 0;
		if (ret > 0)
			end = client->buffer_size - ret;
	}
	if (ret == 0) {
		/* write remaining available data. */
		ret = bluealsa_pcm_client_write_bytes(client,
                                   client->buffer + start, end - start);
		if (ret > 0)
			end -= ret;
	}

	if (ret == -1) {
		/* client has closed connection */
		bluealsa_pcm_client_close_pcm(client);
		bluealsa_pcm_client_set_state(client,
                                           BLUEALSA_PCM_CLIENT_STATE_FINISHED);
		return;
	}

	/* Update buffer pointer ready for next call. */
	if (end >= client->buffer_size)
		end = 0;
	client->out_offset = end;

	/* Set a watch if the write is incomplete. */
	if (ret > 0)
		bluealsa_pcm_client_watch_pcm(client, true);
	else
		bluealsa_pcm_client_watch_pcm(client, false);
}

/**
 * Deliver samples to transport mix. */
void bluealsa_pcm_client_deliver(struct bluealsa_pcm_client *client) {
	struct bluealsa_pcm_multi *multi = client->multi;

	if (client->state != BLUEALSA_PCM_CLIENT_STATE_RUNNING &&
                           client->state != BLUEALSA_PCM_CLIENT_STATE_DRAINING)
		return;

	if (client->in_offset > 0) {
		ssize_t delivered = bluealsa_mix_buffer_add(&multi->playback_buffer,
                       &client->out_offset, client->buffer, client->in_offset);
		if (delivered < 0) {
			debug("client %zu mix underrun", client->id);
			delivered = -delivered;
		}

		if (delivered > 0) {
			memmove(client->buffer, client->buffer + delivered,
                                                client->in_offset - delivered);
			client->in_offset -= delivered;

			/* If the input buffer was full, we now have room for more. */
			if (client->in_offset <=
                 BLUEALSA_MULTI_CLIENT_THRESHOLD * client->multi->period_bytes)
				bluealsa_pcm_client_watch_pcm(client, true);
		}
	}
}

/**
 * Fetch samples from transport. */
void bluealsa_pcm_client_fetch(struct bluealsa_pcm_client *client) {
	struct bluealsa_pcm_multi *multi = client->multi;
	size_t offset = client->in_offset;
	if (offset >= client->buffer_size)
		offset = 0;
	size_t space = client->buffer_size - offset;
	size_t bytes = multi->capture_buffer.len;
	const uint8_t *src = multi->capture_buffer.data;

	if (bytes > space) {
		memcpy(client->buffer + offset, src, space);
		src += space;
		bytes -= space;
		offset = 0;
		if (bytes > client->out_offset) {
			debug("client %zu overrun", client->id);
			client->out_offset = bytes;
		}
	}
	memcpy(client->buffer + offset, src, bytes);

	offset += bytes;
	client->in_offset = offset;
}

/**
 * Action taken when event occurs on client PCM playback connnection. */
static void bluealsa_pcm_client_handle_playback_pcm(
                                          struct bluealsa_pcm_client *client) {

	ssize_t bytes = bluealsa_pcm_client_read(client);
	if (bytes < 0) {
		/* client has closed pcm connection */
		bluealsa_pcm_client_close_pcm(client);
		bluealsa_pcm_client_set_state(client,
                                           BLUEALSA_PCM_CLIENT_STATE_FINISHED);
		return;
	}

	/* If buffer is full, stop reading from FIFO */
	if (bytes == 0)
		bluealsa_pcm_client_watch_pcm(client, false);

	/* Begin adding to mix when sufficient periods are buffered. */
	if (client->state == BLUEALSA_PCM_CLIENT_STATE_IDLE) {
		if (client->in_offset >
                 BLUEALSA_MULTI_CLIENT_THRESHOLD * client->multi->period_bytes)
			bluealsa_pcm_client_set_state(client,
                                            BLUEALSA_PCM_CLIENT_STATE_RUNNING);
	}
}

/**
 * Action taken when event occurs on client PCM capture connnection. */
static void bluealsa_pcm_client_handle_capture_pcm(
                                          struct bluealsa_pcm_client *client) {
	if (client->state != BLUEALSA_PCM_CLIENT_STATE_PAUSED)
		bluealsa_pcm_client_write(client);

	return;
}

/**
 * Action client Drain request.
 *
 * Starts drain timer. */
static void bluealsa_pcm_client_begin_drain(
                                          struct bluealsa_pcm_client *client) {
	debug("DRAIN: client %zu", client->id);
	if (bluealsa_pcm_client_is_playback(client) &&
	                      client->state == BLUEALSA_PCM_CLIENT_STATE_RUNNING) {
		bluealsa_pcm_client_set_state(client,
                                           BLUEALSA_PCM_CLIENT_STATE_DRAINING);
		bluealsa_pcm_client_watch_drain(client, true);
	}
	else {
		if (write(client->control_fd, "OK", 2) != 2)
			error("client control response failed");
	}
}

/**
 * Action client Drop request. */
static void bluealsa_pcm_client_drop(struct bluealsa_pcm_client *client) {
	debug("DROP: client %zu", client->id);
	if (bluealsa_pcm_client_is_playback(client)) {
		bluealsa_pcm_client_watch_pcm(client, false);
		splice(client->pcm_fd, NULL, config.null_fd, NULL, 1024 * 32,
                                                            SPLICE_F_NONBLOCK);
		client->in_offset = 0;
		bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_IDLE);
	}
}

/**
 * Action client Pause request. */
static void bluealsa_pcm_client_pause(struct bluealsa_pcm_client *client) {
	debug("PAUSE: client %zu", client->id);
	struct bluealsa_mix_buffer *buffer = &client->multi->playback_buffer;
	bluealsa_pcm_client_watch_pcm(client, false);
	bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_PAUSED);
	if (bluealsa_pcm_client_is_playback(client))
		client->out_offset = -bluealsa_mix_buffer_delay(buffer,
                                                           client->out_offset);
}

/**
 * Action client Resume request. */
static void bluealsa_pcm_client_resume(struct bluealsa_pcm_client *client) {
	debug("RESUME: client %zu", client->id);
	if (client->state == BLUEALSA_PCM_CLIENT_STATE_IDLE) {
		if (bluealsa_pcm_client_is_playback(client)) {
			bluealsa_pcm_client_watch_pcm(client, true);
			client->out_offset = -2 * client->multi->playback_buffer.period;
		}
		else
			bluealsa_pcm_client_set_state(client,
                                            BLUEALSA_PCM_CLIENT_STATE_RUNNING);
	}
	if (client->state == BLUEALSA_PCM_CLIENT_STATE_PAUSED) {
		bluealsa_pcm_client_set_state(client,
                                        BLUEALSA_PCM_CLIENT_STATE_RUNNING);
		bluealsa_pcm_client_watch_pcm(client, true);
	}
}

/**
 * Action taken when drain timer expires. */
static void bluealsa_pcm_client_handle_drain(
                                         struct bluealsa_pcm_client *client) {
	debug("DRAIN COMPLETE: client %zu", client->id);
	if (client->state != BLUEALSA_PCM_CLIENT_STATE_DRAINING)
		return;

	bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_IDLE);
	bluealsa_pcm_client_watch_drain(client, false);
	client->in_offset = 0;
	if (write(client->control_fd, "OK", 2) != 2)
		error("client control response failed");
}

/**
 * Action taken when event occurs on client control connnection. */
static void bluealsa_pcm_client_handle_control(
                                         struct bluealsa_pcm_client *client) {
	char command[6];
	ssize_t len;
	do {
		len = read(client->control_fd, command, sizeof(command));
	} while (len == -1 && errno == EINTR);

	if (len == -1) {
		if (errno == EAGAIN)
			return;
	}

	if (len <= 0) {
		bluealsa_pcm_client_close_control(client);
		bluealsa_pcm_client_set_state(client,
                                           BLUEALSA_PCM_CLIENT_STATE_FINISHED);
		return;
	}

	if (client->state == BLUEALSA_PCM_CLIENT_STATE_DRAINING) {
	/* Should not happen - a well-behaved client will block during drain.
	 * However, not all clients are well behaved. So we invoke the
	 * drain complete handler before processing this request.*/
		bluealsa_pcm_client_handle_drain(client);
	}

	if (strncmp(command, BLUEALSA_PCM_CTRL_DRAIN, len) == 0) {
		bluealsa_pcm_client_begin_drain(client);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_DROP, len) == 0) {
		bluealsa_pcm_client_drop(client);
		len = write(client->control_fd, "OK", 2);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_PAUSE, len) == 0) {
		bluealsa_pcm_client_pause(client);
		len = write(client->control_fd, "OK", 2);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_RESUME, len) == 0) {
		bluealsa_pcm_client_resume(client);
		len = write(client->control_fd, "OK", 2);
	}
	else {
		warn("Invalid PCM control command: %*s", (int)len, command);
		len = write(client->control_fd, "Invalid", 7);
	}
}

/**
 * Marshall client events.
 * Invokes appropriate action. */
void bluealsa_pcm_client_handle_event(struct bluealsa_pcm_client_event *event) {
	struct bluealsa_pcm_client *client = event->client;
	switch(event->type) {
		case BLUEALSA_EVENT_TYPE_PCM:
			if (bluealsa_pcm_client_is_playback(client))
				bluealsa_pcm_client_handle_playback_pcm(client);
			else
				bluealsa_pcm_client_handle_capture_pcm(client);
			break;
		case BLUEALSA_EVENT_TYPE_CONTROL:
			bluealsa_pcm_client_handle_control(client);
			break;
		case BLUEALSA_EVENT_TYPE_DRAIN:
			bluealsa_pcm_client_handle_drain(client);
			break;
	}
}

void bluealsa_pcm_client_handle_close_event(
                                     struct bluealsa_pcm_client_event *event) {
	struct bluealsa_pcm_client *client = event->client;
	switch (event->type) {
		case BLUEALSA_EVENT_TYPE_PCM:
			bluealsa_pcm_client_close_pcm(client);
			break;
		case BLUEALSA_EVENT_TYPE_CONTROL:
			bluealsa_pcm_client_close_control(client);
			break;
		default:
			g_assert_not_reached();
	}
	bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_FINISHED);
}

/**
 * Allocate a buffer suitable for transport transfer size, and set initial
 * state. */
bool bluealsa_pcm_client_init(struct bluealsa_pcm_client *client) {
	struct bluealsa_pcm_multi *multi = client->multi;

	client->buffer_size =
                   (BLUEALSA_MULTI_CLIENT_THRESHOLD + 1) * multi->period_bytes;

	client->buffer = calloc(client->buffer_size, sizeof(uint8_t));
	if (client->buffer == NULL) {
		error("Unable to allocate client buffer: %s", strerror(errno));
		return false;
	}

	/* Capture clients are active immediately. */
	if (bluealsa_pcm_client_is_capture(client)) {
		bluealsa_pcm_client_set_state(client,
                                            BLUEALSA_PCM_CLIENT_STATE_RUNNING);
	}
	else {
		client->out_offset = -2 * multi->playback_buffer.period;
		bluealsa_pcm_client_watch_pcm(client, true);
	}

	return true;
}

/**
 * Allocate a new client instance. */
struct bluealsa_pcm_client *bluealsa_pcm_client_new(
                struct bluealsa_pcm_multi *multi, int pcm_fd, int control_fd) {
	struct bluealsa_pcm_client *client =
                                 calloc(1, sizeof(struct bluealsa_pcm_client));
	if (!client) {
		error("Unable to create new client: %s", strerror(errno));
		return NULL;
	}

	client->multi = multi;
	client->pcm_fd = pcm_fd;
	client->control_fd = control_fd;
	client->pcm_event.type = BLUEALSA_EVENT_TYPE_PCM;
	client->pcm_event.client = client;

	client->control_event.type = BLUEALSA_EVENT_TYPE_CONTROL;
	client->control_event.client = client;

	struct epoll_event ep_event = {
		.data.ptr = &client->pcm_event,
	 };

	if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD,
                                           client->pcm_fd, &ep_event) == -1) {
		error("Unable to init client, epoll_ctl: %s\n", strerror(errno));
		bluealsa_pcm_client_free(client);
		return NULL;
	}

	ep_event.data.ptr = &client->control_event;
	ep_event.events = EPOLLIN;
	if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, client->control_fd,
                                                           &ep_event) == -1) {
		error("Unable to init client, epoll_ctl: %s\n", strerror(errno));
		epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
		bluealsa_pcm_client_free(client);
		return NULL;
	}

	if (bluealsa_pcm_client_is_playback(client)) {
		client->drain_timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
		client->drain_event.type = BLUEALSA_EVENT_TYPE_DRAIN;
		client->drain_event.client = client;

		ep_event.data.ptr = &client->drain_event;
		ep_event.events = EPOLLIN;
		if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD,
                                   client->drain_timer_fd, &ep_event) == -1) {
			error("Unable to init client, epoll_ctl: %s", strerror(errno));
			epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
			epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->control_fd, NULL);
			bluealsa_pcm_client_free(client);
			return NULL;
		}

	}

	client->watch = false;
	client->state = BLUEALSA_PCM_CLIENT_STATE_IDLE;

	return client;
}

/**
 * Free the resources used by a client. */
void bluealsa_pcm_client_free(struct bluealsa_pcm_client *client) {
	if (bluealsa_pcm_client_is_playback(client)) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL,
                                                 client->drain_timer_fd, NULL);
		close(client->drain_timer_fd);
	}
	bluealsa_pcm_client_close_pcm(client);
	bluealsa_pcm_client_close_control(client);
	bluealsa_pcm_client_set_state(client, BLUEALSA_PCM_CLIENT_STATE_FINISHED);
	free(client->buffer);
	free(client);
}


