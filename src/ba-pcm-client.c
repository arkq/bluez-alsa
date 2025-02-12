/*
 * BlueALSA - ba-pcm-client.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
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
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "ba-config.h"
#include "ba-pcm-client.h"
#include "ba-pcm-mix-buffer.h"
#include "ba-pcm-multi.h"
#include "ba-transport-pcm.h"
#include "bluealsa-iface.h"
#include "shared/log.h"

/* How long to wait for drain to complete, in nanoseconds */
#define BA_PCM_CLIENT_DRAIN_NS 300000000

/* Size of the playback client input buffer */
#define BA_CLIENT_BUFFER_PERIODS (BA_MULTI_CLIENT_THRESHOLD)

static bool ba_pcm_client_is_playback(struct ba_pcm_client *client) {
	return client->multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
}

static bool ba_pcm_client_is_capture(struct ba_pcm_client *client) {
	return client->multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE;
}

/**
 * Calculate offset in mix buffer at which to add initial samples from a client
 * in order to align this client with the delay now reported by the multi */
static size_t ba_pcm_client_playback_init_offset(const struct ba_pcm_client *client) {
	const struct ba_pcm_multi *multi = client->multi;
	const struct ba_mix_buffer *buffer = &multi->playback_buffer;
	const size_t client_samples = client->in_offset * buffer->channels / buffer->frame_size;
	const size_t reported_delay = ba_pcm_mix_buffer_delay(buffer, buffer->end) + (BA_MULTI_CLIENT_THRESHOLD * buffer->period);
	return reported_delay > client_samples ? reported_delay - client_samples : reported_delay;
}

/**
 * Perform side-effects associated with a state change. */
static void ba_pcm_client_set_state(struct ba_pcm_client *client, enum ba_pcm_client_state new_state) {
	pthread_mutex_lock(&client->mutex);
	if (new_state == client->state)
		goto unlock;

	switch (new_state) {
		case BA_PCM_CLIENT_STATE_IDLE:
			client->drain_avail = (size_t) -1;
			/* fallthrough */
		case BA_PCM_CLIENT_STATE_FINISHED:
			if (client->state == BA_PCM_CLIENT_STATE_RUNNING || client->state == BA_PCM_CLIENT_STATE_DRAINING1)
				client->multi->active_count--;
			break;
		case BA_PCM_CLIENT_STATE_PAUSED:
			if (client->state == BA_PCM_CLIENT_STATE_RUNNING && ba_pcm_client_is_capture(client))
				client->multi->active_count--;
			break;
		case BA_PCM_CLIENT_STATE_RUNNING:
			if (ba_pcm_client_is_capture(client)) {
				if (client->state == BA_PCM_CLIENT_STATE_IDLE || client->state == BA_PCM_CLIENT_STATE_INIT || client->state == BA_PCM_CLIENT_STATE_PAUSED)
					client->multi->active_count++;
			}
			else {
				if (client->state == BA_PCM_CLIENT_STATE_IDLE) {
					client->out_offset = -ba_pcm_client_playback_init_offset(client);
					client->multi->active_count++;
				}
				else if (client->state == BA_PCM_CLIENT_STATE_DRAINING1)
					goto unlock;
			}
			break;
		case BA_PCM_CLIENT_STATE_DRAINING1:
			break;
		case BA_PCM_CLIENT_STATE_DRAINING2:
			if (client->state == BA_PCM_CLIENT_STATE_DRAINING1)
				client->multi->active_count--;
			break;
		default:
			/* not reached */
			break;
	}
	client->state = new_state;

unlock:
	pthread_mutex_unlock(&client->mutex);
}

/**
 * Clean up resources associated with a client PCM connection. */
static void ba_pcm_client_close_pcm(struct ba_pcm_client *client) {
	if (client->pcm_fd != -1) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
		client->watch = false;
		pthread_mutex_lock(&client->mutex);
		close(client->pcm_fd);
		client->pcm_fd = -1;
		pthread_mutex_unlock(&client->mutex);
	}
}

/**
 * Clean up resources associated with a client control connection. */
static void ba_pcm_client_close_control(
                                          struct ba_pcm_client *client) {
	if (client->control_fd != -1) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL, client->control_fd, NULL);
		close(client->control_fd);
		client->control_fd = -1;
	}
}

/**
 * Start/stop watching for PCM i/o events. */
static void ba_pcm_client_watch_pcm(
                            struct ba_pcm_client *client, bool enabled) {
	if (client->watch == enabled)
		return;

	const uint32_t type = ba_pcm_client_is_playback(client) ? EPOLLIN : EPOLLOUT;
	struct epoll_event event = {
		.events = enabled ? type : 0,
		.data.ptr = &client->pcm_event,
	};
	epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_MOD, client->pcm_fd, &event);
	client->watch = enabled;
}

/**
 * Start/stop watching for drain timer expiry event. */
static void ba_pcm_client_watch_drain(struct ba_pcm_client *client, bool enabled) {
	struct itimerspec timeout = {
		.it_interval = { 0 },
		.it_value = {
			.tv_sec = 0,
			.tv_nsec = enabled ? BA_PCM_CLIENT_DRAIN_NS : 0,
		},
	};
	timerfd_settime(client->drain_timer_fd, 0, &timeout, NULL);
}

/**
 * Read bytes from FIFO.
 * @return number of bytes read, 0 if buffer full, or -1 if client closed pipe */
static ssize_t ba_pcm_client_read(struct ba_pcm_client *client) {

	const size_t space = client->buffer_size - client->in_offset;
	if (space == 0)
		return 0;

	uint8_t *buf = client->buffer + client->in_offset;

	ssize_t bytes;
	while ((bytes = read(client->pcm_fd, buf, space)) == -1 && errno == EINTR)
		continue;

	/* pipe closed by remote end */
	if (bytes == 0)
		return -1;

	/* FIFO may be empty but client still open. */
	if (bytes == -1 && errno == EAGAIN)
		bytes = 0;

	if (bytes > 0)
		client->in_offset += bytes;

	return bytes;
}

/**
 * Write samples to the client fifo
 */
void ba_pcm_client_write(struct ba_pcm_client *client, const void *buffer, size_t samples) {
	const uint8_t *buffer_ = buffer;
	size_t len = samples * BA_TRANSPORT_PCM_FORMAT_BYTES(client->multi->pcm->format);
	ssize_t ret;

	do {
		pthread_mutex_lock(&client->mutex);
		int fd = client->pcm_fd;
		if (fd == -1) {
			pthread_mutex_unlock(&client->mutex);
			return;
		}
		ret = write(fd, buffer_, len);
		pthread_mutex_unlock(&client->mutex);

		if (ret == -1)
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
				/* If the client is so slow that the FIFO fills up, then it
				 * is inevitable that audio frames will be eventually be
				 * dropped in the bluetooth controller if we block here.
				 * It is better that we discard frames here so that the
				 * decoder is not interrupted. */
				warn("Dropping PCM frames: %s", "PCM overrun");
				ret = len;
				break;
			default:
				/* The client has closed the pipe, or an unrecoverable error
				 * has occurred. */
				ba_pcm_client_close_pcm(client);
				ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_FINISHED);
				return;
			}

		buffer_ += ret;
		len -= ret;

	} while (len != 0);

}

/**
 * Deliver samples to transport mix. */
void ba_pcm_client_deliver(struct ba_pcm_client *client) {
	struct ba_pcm_multi *multi = client->multi;

	if (client->state != BA_PCM_CLIENT_STATE_RUNNING &&
                           client->state != BA_PCM_CLIENT_STATE_DRAINING1)
		return;

	if (client->state == BA_PCM_CLIENT_STATE_DRAINING1) {
		ssize_t bytes = ba_pcm_client_read(client);
		if (bytes < 0) {
			/* client has closed pcm connection */
			ba_pcm_client_close_pcm(client);
			ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_FINISHED);
			return;
		}
		if (client->in_offset == 0 && errno == EAGAIN) {
			const size_t mix_avail = ba_pcm_mix_buffer_calc_avail(&multi->playback_buffer, multi->playback_buffer.mix_offset, client->out_offset);
			if (mix_avail == 0 || mix_avail > client->drain_avail) {
				/* The mix buffer has completely drained all frames from
				 * this client. We now wait some time for the bluetooth system
				 * to play out all sent frames*/
				ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_DRAINING2);
				ba_pcm_client_watch_drain(client, true);
				return;
			}
			else
				client->drain_avail = mix_avail;
		}
	}

	if (client->in_offset > 0) {
		ssize_t delivered = ba_pcm_mix_buffer_add(&multi->playback_buffer, &client->out_offset, client->buffer, client->in_offset);
		if (delivered > 0) {
			memmove(client->buffer, client->buffer + delivered, client->in_offset - delivered);
			client->in_offset -= delivered;

			/* If the input buffer was full, we now have room for more. */
			ba_pcm_client_watch_pcm(client, true);
		}
	}
}

/**
 * Action taken when event occurs on client PCM playback connection. */
static void ba_pcm_client_handle_playback_pcm(struct ba_pcm_client *client) {

	ssize_t bytes = ba_pcm_client_read(client);
	if (bytes < 0) {
		/* client has closed pcm connection */
		ba_pcm_client_close_pcm(client);
		ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_FINISHED);
		return;
	}

	/* If buffer is full, stop reading from FIFO */
	if (bytes == 0)
		ba_pcm_client_watch_pcm(client, false);

	/* Begin adding to mix when sufficient periods are buffered. */
	if (client->state == BA_PCM_CLIENT_STATE_IDLE) {
		if (client->in_offset >= BA_MULTI_CLIENT_THRESHOLD * client->multi->period_bytes)
			ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_RUNNING);
	}
}

/**
 * Action client Drain request.
 *
 * Starts drain timer. */
static void ba_pcm_client_begin_drain(
                                          struct ba_pcm_client *client) {
	debug("DRAIN: client %zu", client->id);
	if (ba_pcm_client_is_playback(client) && client->state == BA_PCM_CLIENT_STATE_RUNNING) {
		ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_DRAINING1);
		ba_pcm_client_watch_pcm(client, false);
	}
	else {
		if (write(client->control_fd, "OK", 2) != 2)
			error("client control response failed");
	}
}

/**
 * Action client Drop request. */
static void ba_pcm_client_drop(struct ba_pcm_client *client) {
	debug("DROP: client %zu", client->id);
	if (ba_pcm_client_is_playback(client)) {
		ba_pcm_client_watch_drain(client, false);
		splice(client->pcm_fd, NULL, config.null_fd, NULL, 1024 * 32, SPLICE_F_NONBLOCK);
		client->in_offset = 0;
		ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_IDLE);
		client->drop = true;
	}
}

/**
 * Action client Pause request. */
static void ba_pcm_client_pause(struct ba_pcm_client *client) {
	debug("PAUSE: client %zu", client->id);
	ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_PAUSED);
	ba_pcm_client_watch_pcm(client, false);
	if (ba_pcm_client_is_playback(client)) {
		struct ba_mix_buffer *buffer = &client->multi->playback_buffer;
		client->out_offset = -ba_pcm_mix_buffer_delay(buffer, client->out_offset);
	}
}

/**
 * Action client Resume request. */
static void ba_pcm_client_resume(struct ba_pcm_client *client) {
	debug("RESUME: client %zu", client->id);
	if (client->state == BA_PCM_CLIENT_STATE_IDLE) {
		if (ba_pcm_client_is_playback(client)) {
			ba_pcm_client_watch_pcm(client, true);
			client->drop = false;
		}
		else
			ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_RUNNING);
	}
	if (client->state == BA_PCM_CLIENT_STATE_PAUSED) {
		ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_RUNNING);
		if (ba_pcm_client_is_playback(client))
			ba_pcm_client_watch_pcm(client, true);
	}
}

/**
 * Action taken when drain timer expires. */
static void ba_pcm_client_handle_drain(struct ba_pcm_client *client) {
	debug("DRAIN COMPLETE: client %zu", client->id);
	if (client->state != BA_PCM_CLIENT_STATE_DRAINING2)
		return;

	ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_IDLE);
	ba_pcm_client_watch_drain(client, false);
	ba_pcm_client_watch_pcm(client, true);
	client->in_offset = 0;
	if (write(client->control_fd, "OK", 2) != 2)
		error("client control response failed");
}

/**
 * Action taken when event occurs on client control connection. */
static void ba_pcm_client_handle_control(struct ba_pcm_client *client) {
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
		ba_pcm_client_close_control(client);
		ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_FINISHED);
		return;
	}

	if (client->state == BA_PCM_CLIENT_STATE_DRAINING1 ||
			client->state == BA_PCM_CLIENT_STATE_DRAINING2) {
	/* Should not happen - a well-behaved client will block during drain.
	 * However, not all clients are well behaved. So we invoke the
	 * drain complete handler before processing this request.*/
		ba_pcm_client_handle_drain(client);
	}

	if (strncmp(command, BLUEALSA_PCM_CTRL_DRAIN, len) == 0) {
		ba_pcm_client_begin_drain(client);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_DROP, len) == 0) {
		ba_pcm_client_drop(client);
		len = write(client->control_fd, "OK", 2);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_PAUSE, len) == 0) {
		ba_pcm_client_pause(client);
		len = write(client->control_fd, "OK", 2);
	}
	else if (strncmp(command, BLUEALSA_PCM_CTRL_RESUME, len) == 0) {
		ba_pcm_client_resume(client);
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
void ba_pcm_client_handle_event(struct ba_pcm_client_event *event) {
	struct ba_pcm_client *client = event->client;
	switch(event->type) {
		case BA_EVENT_TYPE_PCM:
			if (ba_pcm_client_is_playback(client))
				ba_pcm_client_handle_playback_pcm(client);
			break;
		case BA_EVENT_TYPE_CONTROL:
			ba_pcm_client_handle_control(client);
			break;
		case BA_EVENT_TYPE_DRAIN:
			ba_pcm_client_handle_drain(client);
			break;
	}
}

void ba_pcm_client_handle_close_event(
                                     struct ba_pcm_client_event *event) {
	struct ba_pcm_client *client = event->client;
	switch (event->type) {
		case BA_EVENT_TYPE_PCM:
			ba_pcm_client_close_pcm(client);
			break;
		case BA_EVENT_TYPE_CONTROL:
			ba_pcm_client_close_control(client);
			break;
		default:
			g_assert_not_reached();
	}
	ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_FINISHED);
}

/**
 * Called when a running playback pcm fails to transfer audio frames in time
 * to prevent mix buffer becoming empty. */
void ba_pcm_client_underrun(struct ba_pcm_client *client) {
	if (client->state == BA_PCM_CLIENT_STATE_RUNNING) {
		ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_IDLE);
		debug("client %zu underrun", client->id);
	}
}

/**
 * Allocate a buffer suitable for transport transfer size, and set initial
 * state. */
bool ba_pcm_client_init(struct ba_pcm_client *client) {
	struct ba_pcm_multi *multi = client->multi;

	if (ba_pcm_client_is_playback(client)) {
		client->buffer_size = BA_CLIENT_BUFFER_PERIODS * multi->period_bytes;

		client->buffer = calloc(client->buffer_size, sizeof(uint8_t));
		if (client->buffer == NULL) {
			error("Unable to allocate client buffer: %s", strerror(errno));
			return false;
		}

		ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_IDLE);
		ba_pcm_client_watch_pcm(client, true);
	}
	else {
		/* Capture clients are active immediately. */
		ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_RUNNING);
	}

	return true;
}

/**
 * Allocate a new client instance. */
struct ba_pcm_client *ba_pcm_client_new(struct ba_pcm_multi *multi, int pcm_fd, int control_fd) {
	struct ba_pcm_client *client = calloc(1, sizeof(struct ba_pcm_client));
	if (!client) {
		error("Unable to create new client: %s", strerror(errno));
		return NULL;
	}

	client->multi = multi;
	client->pcm_fd = pcm_fd;
	client->control_fd = control_fd;
	client->drain_timer_fd = -1;
	pthread_mutex_init(&client->mutex, NULL);

	client->pcm_event.type = BA_EVENT_TYPE_PCM;
	client->pcm_event.client = client;
	client->control_event.type = BA_EVENT_TYPE_CONTROL;
	client->control_event.client = client;

	struct epoll_event ep_event = {
		.data.ptr = &client->pcm_event,
	 };

	if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, client->pcm_fd, &ep_event) == -1) {
		error("Unable to init client, epoll_ctl: %s\n", strerror(errno));
		ba_pcm_client_free(client);
		return NULL;
	}

	ep_event.data.ptr = &client->control_event;
	ep_event.events = EPOLLIN;
	if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, client->control_fd, &ep_event) == -1) {
		error("Unable to init client, epoll_ctl: %s\n", strerror(errno));
		epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
		ba_pcm_client_free(client);
		return NULL;
	}

	if (ba_pcm_client_is_playback(client)) {
		client->drain_timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
		client->drain_event.type = BA_EVENT_TYPE_DRAIN;
		client->drain_event.client = client;

		ep_event.data.ptr = &client->drain_event;
		ep_event.events = EPOLLIN;
		if (epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, client->drain_timer_fd, &ep_event) == -1) {
			error("Unable to init client, epoll_ctl: %s", strerror(errno));
			epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->pcm_fd, NULL);
			epoll_ctl(multi->epoll_fd, EPOLL_CTL_DEL, client->control_fd, NULL);
			ba_pcm_client_free(client);
			return NULL;
		}

	}

	client->watch = false;
	client->state = BA_PCM_CLIENT_STATE_INIT;

	return client;
}

/**
 * Free the resources used by a client. */
void ba_pcm_client_free(struct ba_pcm_client *client) {
	if (ba_pcm_client_is_playback(client)) {
		epoll_ctl(client->multi->epoll_fd, EPOLL_CTL_DEL, client->drain_timer_fd, NULL);
		if (client->drain_timer_fd >= 0)
			close(client->drain_timer_fd);
		free(client->buffer);
	}
	ba_pcm_client_close_pcm(client);
	ba_pcm_client_close_control(client);
	ba_pcm_client_set_state(client, BA_PCM_CLIENT_STATE_FINISHED);
	pthread_mutex_destroy(&client->mutex);
	free(client);
}
