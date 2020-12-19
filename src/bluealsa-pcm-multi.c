/*
 * BlueALSA - bluealsa-pcm-multi.c
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

#include <alloca.h>
#include <errno.h>
#include <glib.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "a2dp-codecs.h"
#include "bluealsa.h"
#include "bluealsa-pcm-multi.h"
#include "bluealsa-pcm-client.h"
#include "ba-transport.h"
#include "shared/log.h"
#include "shared/defs.h"


/* Limit number of clients to ensure sufficient resources are available. */
#define BLUEALSA_MULTI_MAX_CLIENTS 32

/* Size of epoll event array. Allow for client control, pcm, and drain timer,
 * plus the mix event fd. */
#define BLUEALSA_MULTI_MAX_EVENTS (1 + BLUEALSA_MULTI_MAX_CLIENTS * 3)

/* Determines the size of the mix buffer. */
#define BLUEALSA_MULTI_BUFFER_PERIODS 16

/* Number of periods to hold in mix before starting playback. */
#define BLUEALSA_MULTI_MIX_THRESHOLD 4

static void *bluealsa_pcm_mix_thread_func(struct bluealsa_pcm_multi *multi);
static void *bluealsa_pcm_snoop_thread_func(struct bluealsa_pcm_multi *multi);
static bool bluealsa_pcm_multi_remove_client(struct bluealsa_pcm_multi *multi,
                                           struct bluealsa_pcm_client *client);


static bool bluealsa_pcm_multi_is_capture(struct bluealsa_pcm_multi *multi) {
	return multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE;
}

static bool bluealsa_pcm_multi_is_playback(struct bluealsa_pcm_multi *multi) {
	return multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
}

/**
 * Is multi-client support implemented and configured for the given pcm ? */
bool bluealsa_pcm_multi_enabled(struct ba_transport_pcm *pcm) {
	if (!config.multi_enabled)
		return false;

	if (pcm->t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return pcm->t->a2dp.pcm.format != BA_TRANSPORT_PCM_FORMAT_S24_3LE &&
                    pcm->t->a2dp.pcm.format != BA_TRANSPORT_PCM_FORMAT_S24_4LE;

	return true;
}

/**
 * Create multi-client support for the given transport pcm. */
struct bluealsa_pcm_multi *bluealsa_pcm_multi_create(
                                                struct ba_transport_pcm *pcm) {
	struct bluealsa_pcm_multi *multi = calloc(1,
                                            sizeof(struct bluealsa_pcm_multi));
	if (multi == NULL)
		return multi;

	multi->pcm = pcm;
	multi->thread = config.main_thread;

	pthread_mutex_init(&multi->mutex, NULL);
	pthread_cond_init(&multi->cond, NULL);

	if ((multi->epoll_fd = epoll_create(1)) == -1)
		goto fail;

	if ((multi->event_fd = eventfd(0, 0)) == -1)
		goto fail;

	return multi;

fail:
	if (multi->epoll_fd != -1)
		close(multi->epoll_fd);
	if (multi->event_fd != -1)
		close(multi->event_fd);
	if (multi->pcm->fd != -1) {
		close(multi->pcm->fd);
		multi->pcm->fd = -1;
	}
	free(multi);
	return NULL;
}

static void bluealsa_pcm_multi_init_clients(struct bluealsa_pcm_multi *multi) {
	pthread_mutex_lock(&multi->mutex);
	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct bluealsa_pcm_client *client = el->data;
		if (client->buffer == NULL) {
			if (!bluealsa_pcm_client_init(client))
				bluealsa_pcm_multi_remove_client(client->multi, client);
		}
	}
	pthread_mutex_unlock(&multi->mutex);
}

/**
 * Initialize multi-client support.
 *
 * Set up the buffer parameters and enable client audio I/O.
 *
 * @param multi The multi-client instance to be initialized.
 * @param transfer_samples The largest number of samples that will be passed
 *                         between the transport I/O thread and the client
 *                         thread in a single transfer.
 * @return true if multi-client successfully initialized. */
bool bluealsa_pcm_multi_init(struct bluealsa_pcm_multi *multi,
                                                     size_t transfer_samples) {

	debug("Initializing multi client support");

	size_t period_frames = transfer_samples / multi->pcm->channels;
	multi->period_bytes = period_frames * multi->pcm->channels * BA_TRANSPORT_PCM_FORMAT_BYTES(multi->pcm->format);

	if (bluealsa_pcm_multi_is_playback(multi)) {
		size_t buffer_frames = BLUEALSA_MULTI_BUFFER_PERIODS * period_frames;
		if (bluealsa_mix_buffer_init(&multi->playback_buffer,
                                      multi->pcm->format, multi->pcm->channels,
                                      buffer_frames, period_frames) == -1)
			return false;
	}

	multi->start_threshold =
           BLUEALSA_MULTI_MIX_THRESHOLD * period_frames * multi->pcm->channels;

	debug("period bytes = %zu, start threshold = %zu",
                                  multi->period_bytes, multi->start_threshold);
	bluealsa_pcm_multi_init_clients(multi);

	return true;
}

/**
 * Stop the multi-client support thread. */
void bluealsa_pcm_multi_reset(struct bluealsa_pcm_multi *multi) {
	if (multi->thread != config.main_thread) {
		eventfd_write(multi->event_fd, 0xDEAD0000);
		pthread_join(multi->thread, NULL);
		multi->thread = config.main_thread;
	}

	if (bluealsa_pcm_multi_is_playback(multi) &&
                                              multi->playback_buffer.size > 0)
		bluealsa_mix_buffer_reset(&multi->playback_buffer);

	while (multi->client_count > 0)
		bluealsa_pcm_multi_remove_client(multi,
                                           g_list_first(multi->clients)->data);
	multi->client_count = 0;
	multi->state = BLUEALSA_PCM_MULTI_STATE_INIT;
}

/**
 * Release the resources used by a multi. */
void bluealsa_pcm_multi_free(struct bluealsa_pcm_multi *multi) {
	bluealsa_pcm_multi_reset(multi);

	g_list_free(multi->clients);

	if (bluealsa_pcm_multi_is_playback(multi))
		bluealsa_mix_buffer_release(&multi->playback_buffer);

	close(multi->epoll_fd);
	close(multi->event_fd);

	pthread_mutex_destroy(&multi->mutex);
	pthread_cond_destroy(&multi->cond);

	free(multi);
}

/**
 * Start the multi client thread. */
static bool bluealsa_pcm_multi_start(struct bluealsa_pcm_multi *multi) {

	if ((multi->pcm->fd != -1) || (multi->pcm->fd = eventfd(0, 0)) == -1)
		return false;

	if (bluealsa_pcm_multi_is_playback(multi)) {
		if (pthread_create(&multi->thread, NULL,
		         PTHREAD_ROUTINE(bluealsa_pcm_mix_thread_func), multi) == -1) {
			error("Cannot create pcm multi mix thread: %s", strerror(errno));
			bluealsa_mix_buffer_release(&multi->playback_buffer);
			multi->thread = config.main_thread;
			return false;
		}
		pthread_setname_np(multi->thread, "ba-pcm-mix");
	}
	else {
		if (pthread_create(&multi->thread, NULL,
		       PTHREAD_ROUTINE(bluealsa_pcm_snoop_thread_func), multi) == -1) {
			error("Cannot create pcm multi snoop thread: %s", strerror(errno));
			multi->thread = config.main_thread;
			return false;
		}
		pthread_setname_np(multi->thread, "ba-pcm-snoop");
	}

	return true;
}

/**
 * Include a new client stream.
 *
 * Starts the multi thread if not already running.
 *
 * @param multi The multi to which the client is to be added.
 * @param pcm_fd File descriptor for client audio i/o.
 * @param control_fd File descriptor for client control commands.
 * @return true if successful.
 */
bool bluealsa_pcm_multi_add_client(struct bluealsa_pcm_multi *multi,
                                                  int pcm_fd, int control_fd) {
	if (multi->client_count == BLUEALSA_MULTI_MAX_CLIENTS)
		return false;

	if (bluealsa_pcm_multi_is_capture(multi) &&
                           multi->state == BLUEALSA_PCM_MULTI_STATE_FINISHED) {
		/* client thread has failed - clean it up before starting new one. */
		bluealsa_pcm_multi_reset(multi);
	}

	if (multi->thread == config.main_thread && !bluealsa_pcm_multi_start(multi))
		return false;

	struct bluealsa_pcm_client *client = bluealsa_pcm_client_new(multi,
                                                           pcm_fd, control_fd);
	if (!client)
		return false;

	pthread_mutex_lock(&multi->mutex);

	/* Postpone initialization of client if multi itself is not yet
	 * initialized. */
	if (multi->period_bytes > 0) {
		if (!bluealsa_pcm_client_init(client)) {
			bluealsa_pcm_client_free(client);
			pthread_mutex_unlock(&multi->mutex);
			return false;
		}
	}

	multi->clients = g_list_prepend(multi->clients, client);
	multi->client_count++;

	if (bluealsa_pcm_multi_is_playback(multi)) {
		if (multi->state == BLUEALSA_PCM_MULTI_STATE_FINISHED)
			multi->state = BLUEALSA_PCM_MULTI_STATE_INIT;
	}
	else {
		if (multi->state == BLUEALSA_PCM_MULTI_STATE_INIT)
			multi->state = BLUEALSA_PCM_MULTI_STATE_RUNNING;
	}

#if DEBUG
	client->id = ++multi->client_no;
#endif

	if (multi->client_count == 1)
		ba_transport_thread_send_signal(multi->pcm->th, BA_TRANSPORT_SIGNAL_PCM_OPEN);

	pthread_mutex_unlock(&multi->mutex);

	debug("new client id %zu, total clients now %zu", client->id,
                                                          multi->client_count);
	return true;
}

/* Remove a client stream.
 * @return false if no clients remain, true otherwise. */
static bool bluealsa_pcm_multi_remove_client(struct bluealsa_pcm_multi *multi,
                                          struct bluealsa_pcm_client *client) {
	bool res;

	client->multi->clients = g_list_remove(multi->clients, client);
	res = (--client->multi->client_count > 0);

	debug("removed client no %zu, total clients now %zu",
                                              client->id, multi->client_count);
	bluealsa_pcm_client_free(client);

	return res;
}

/**
 * Copy samples to client buffers, and trigger client thread to write to each
 * client.
 *
 * Called by the transport I/O thread.
 * @param multi Pointer to the multi.
 * @param buffer Pointer to the buffer from which to obtain the samples.
 * @param samples the number of samples available in the decoder buffer. */
void bluealsa_pcm_multi_write(struct bluealsa_pcm_multi *multi,
                                                void *buffer, size_t samples) {

	pthread_mutex_lock(&multi->mutex);

	multi->capture_buffer.data = buffer,
	multi->capture_buffer.len =
                   samples * BA_TRANSPORT_PCM_FORMAT_BYTES(multi->pcm->format);

	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct bluealsa_pcm_client *client = el->data;
		if (client->state == BLUEALSA_PCM_CLIENT_STATE_RUNNING)
			bluealsa_pcm_client_fetch(client);
	}

	/* ping client thread to write out new data */
	eventfd_write(multi->event_fd, 1);

	pthread_mutex_unlock(&multi->mutex);
}

/**
 * Read mixed samples.
 *
 * multi client replacement for ba_transport_pcm_read() */
ssize_t bluealsa_pcm_multi_read(struct bluealsa_pcm_multi *multi,
                                               void *buffer, size_t samples) {
	enum bluealsa_pcm_multi_state state = multi->state;

	/* Block until data is available. */
	eventfd_t value = 0;
	eventfd_read(multi->pcm->fd, &value);

	if (state == BLUEALSA_PCM_MULTI_STATE_FINISHED) {
		ba_transport_pcm_release(multi->pcm);
		return 0;
	}
	if (state != BLUEALSA_PCM_MULTI_STATE_RUNNING ||
                           bluealsa_mix_buffer_empty(&multi->playback_buffer)) {
		errno = EAGAIN;
		return -1;
	}

	double scale_array[2];
	scale_array[0] = multi->pcm->volume[0].muted ? 0.0 :
                            pow(10, (0.01 * multi->pcm->volume[0].level) / 20);
	if (multi->pcm->channels == 2)
		scale_array[1] = multi->pcm->volume[1].muted ? 0.0 :
                            pow(10, (0.01 * multi->pcm->volume[1].level) / 20);

	samples = bluealsa_mix_buffer_read(&multi->playback_buffer,
                                                 buffer, samples, scale_array);

	/* Trigger client thread to re-fill the mix. */
	eventfd_write(multi->event_fd, 1);

	return samples;
}

/**
 * Write out samples to clients.
 * Return false if a client is removed. */
static bool bluealsa_pcm_multi_deliver(struct bluealsa_pcm_multi *multi) {
	GList *el;
	bool deleted = false;

	for (el = multi->clients; el != NULL; el = el->next) {
		struct bluealsa_pcm_client *client = el->data;

		if (client->state == BLUEALSA_PCM_CLIENT_STATE_RUNNING)
			bluealsa_pcm_client_write(client);

		if (client->state == BLUEALSA_PCM_CLIENT_STATE_FINISHED) {
			if (!bluealsa_pcm_multi_remove_client(multi, client))
				/* there are no more clients remaining */
				multi->state = BLUEALSA_PCM_MULTI_STATE_FINISHED;
			deleted = true;
		}
	}
	return deleted;
}

/**
 * Signal the transport i/o thread that mixed samples are available. */
static void bluealsa_pcm_multi_wake_transport(
                                            struct bluealsa_pcm_multi *multi) {
	eventfd_write(multi->pcm->fd, 1);
}

/**
 * Add more samples from clients into the mix. */
static void bluealsa_pcm_multi_update_mix(struct bluealsa_pcm_multi *multi) {
	GList *el;
	pthread_mutex_lock(&multi->mutex);
	for (el = multi->clients; el != NULL; el = el->next) {
		struct bluealsa_pcm_client *client = el->data;
		bluealsa_pcm_client_deliver(client);
	}
	pthread_mutex_unlock(&multi->mutex);
}

static void bluealsa_pcm_multi_update_mix_delay(struct bluealsa_pcm_multi *multi) {
	size_t delay_frames = 0;

	if (multi->state == BLUEALSA_PCM_MULTI_STATE_RUNNING) {
		/* As each client may have different buffer fill levels, we can only
		 * provide an overall approximation of the actual delay caused by
		 * mixing. */
		size_t period_frames = multi->period_bytes / multi->playback_buffer.frame_size;
		delay_frames = period_frames * (BLUEALSA_MULTI_MIX_THRESHOLD + BLUEALSA_MULTI_CLIENT_THRESHOLD);
	}
	else {
		/* To avoid reporting a large change in delay when the first playback
		 * client starts, we calculate the actual frames buffered by the client.
		 */
		if (multi->client_count > 0) {
			struct bluealsa_pcm_client *client1 = multi->clients->data;
			delay_frames = bluealsa_mix_buffer_avail(&multi->playback_buffer);
			delay_frames += client1->in_offset;
		}
	}

	multi->delay = delay_frames * 10000 / multi->pcm->sampling;
}

/**
 * The mix buffer is ready for reading. */
static bool bluealsa_pcm_multi_mix_ready(struct bluealsa_pcm_multi *multi) {
	struct bluealsa_mix_buffer *buffer = &multi->playback_buffer;
	return bluealsa_mix_buffer_avail(buffer) > multi->start_threshold;
}

/**
 * The mix thread. */
static void *bluealsa_pcm_mix_thread_func(struct bluealsa_pcm_multi *multi) {

	struct epoll_event events[BLUEALSA_MULTI_MAX_EVENTS] = { 0 };

	struct epoll_event event = {
		.events =  EPOLLIN,
		.data.ptr = multi,
	};

	epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, multi->event_fd, &event);

	debug("Starting pcm mix loop");
	for (;;) {

		int event_count;
		do {
			event_count = epoll_wait(multi->epoll_fd, events,
                                               BLUEALSA_MULTI_MAX_EVENTS, -1);
		} while (event_count == -1 && errno == EINTR);

		if (event_count <= 0) {
			error("epoll_wait failed: %d (%s)", errno, strerror(errno));
			goto terminate;
		}

		int n;
		for (n = 0; n < event_count; n++) {

			if (events[n].data.ptr == multi) {
				/* trigger from transport thread */
				eventfd_t value = 0;
				eventfd_read(multi->event_fd, &value);
				if (value >= 0xDEAD0000)
					goto terminate;

				/* add buffered client audio to mix */
				bluealsa_pcm_multi_update_mix(multi);
			}

			else {   /* client event */
				struct bluealsa_pcm_client_event *event = events[n].data.ptr;
				struct bluealsa_pcm_client *client = event->client;

				bluealsa_pcm_client_handle_event(event);

				if (client->state == BLUEALSA_PCM_CLIENT_STATE_FINISHED) {
					pthread_mutex_lock(&multi->mutex);
					if (!bluealsa_pcm_multi_remove_client(multi, client)) {
						/* Last client has closed. */
						multi->state = BLUEALSA_PCM_MULTI_STATE_FINISHED;
						bluealsa_pcm_multi_wake_transport(multi);
					}
					pthread_mutex_unlock(&multi->mutex);

					/* removing a client invalidates the event array, so
					 * we need to call epoll() again here */
					break;
				}
			}
		}

		if (multi->client_count == 0)
			continue;

		else if (multi->state == BLUEALSA_PCM_MULTI_STATE_RUNNING) {
			if (bluealsa_pcm_multi_mix_ready(multi))
				bluealsa_pcm_multi_wake_transport(multi);
			else
				multi->state = BLUEALSA_PCM_MULTI_STATE_INIT;
		}
		else if (multi->state == BLUEALSA_PCM_MULTI_STATE_INIT) {
			bluealsa_pcm_multi_update_mix(multi);
			if (bluealsa_pcm_multi_mix_ready(multi)) {
				multi->state = BLUEALSA_PCM_MULTI_STATE_RUNNING;
				bluealsa_pcm_multi_update_mix_delay(multi);
				bluealsa_pcm_multi_wake_transport(multi);
			}
			else
				bluealsa_pcm_multi_update_mix_delay(multi);
		}
	}

terminate:
	multi->state = BLUEALSA_PCM_MULTI_STATE_FINISHED;
	bluealsa_pcm_multi_wake_transport(multi);
	debug("mix thread func terminated");
	return NULL;
}


/**
 * The snoop thread. */
static void *bluealsa_pcm_snoop_thread_func(struct bluealsa_pcm_multi *multi) {

	struct epoll_event events[BLUEALSA_MULTI_MAX_EVENTS];

	struct epoll_event event = {
		.events =  EPOLLIN,
		.data.ptr = multi,
	};

	epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, multi->event_fd, &event);

	debug("Starting pcm snoop loop");
	for (;;) {
		int ret;

		do {
			ret = epoll_wait(multi->epoll_fd, events,
			                                     BLUEALSA_MULTI_MAX_EVENTS, -1);
		} while (ret == -1 && errno == EINTR);

		if (ret <= 0) {
			error("epoll_wait failed: %d (%s)", errno, strerror(errno));
			goto terminate;
		}

		int n;
		for (n = 0; n < ret; n++) {

			if (events[n].data.ptr == multi) {
				/* trigger from transport thread */

				pthread_mutex_lock(&multi->mutex);

				eventfd_t value = 0;
				eventfd_read(multi->event_fd, &value);
				if (value >= 0xDEAD0000) {
					pthread_mutex_unlock(&multi->mutex);
					goto terminate;
				}

				/* copy audio samples to the clients */
				bool deleted = bluealsa_pcm_multi_deliver(multi);
				if (multi->active_count == 0) {
					multi->state = BLUEALSA_PCM_MULTI_STATE_PAUSED;
					ba_transport_thread_send_signal(multi->pcm->th,
                                                BA_TRANSPORT_SIGNAL_PCM_PAUSE);
				}

				pthread_mutex_unlock(&multi->mutex);

				if (deleted)
					/* the event array will be invalid if any clients were
					 * deleted by the above call to bluealsa_pcm_multi_deliver()
					 * So we must call epoll() again. */
					break;
			}

			else {
				/* client event */
				struct bluealsa_pcm_client_event *event = events[n].data.ptr;
				if (events[n].events & (EPOLLHUP|EPOLLERR)) {
					bluealsa_pcm_client_handle_close_event(event);
					pthread_mutex_lock(&multi->mutex);
					bluealsa_pcm_multi_remove_client(multi, event->client);
					pthread_mutex_unlock(&multi->mutex);

					/* removing a client invalidates the event array, so
					 * we need to call epoll() again here */
					break;
				}
				else {
					bluealsa_pcm_client_handle_event(event);
					if (multi->state == BLUEALSA_PCM_MULTI_STATE_PAUSED &&
                                                     multi->active_count > 0) {
						multi->state = BLUEALSA_PCM_MULTI_STATE_RUNNING;
						ba_transport_thread_send_signal(multi->pcm->th,
                                               BA_TRANSPORT_SIGNAL_PCM_RESUME);
					}
				}
			}
		}
	}

terminate:
	multi->state = BLUEALSA_PCM_MULTI_STATE_FINISHED;
	debug("snoop thread func terminated");
	return NULL;
}

