/*
 * BlueALSA - ba-pcm-multi.c
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
#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "ba-config.h"
#include "ba-pcm-client.h"
#include "ba-pcm-multi.h"
#include "ba-transport-pcm.h"
#include "ba-transport.h"
#include "shared/log.h"
#include "shared/defs.h"


/* Limit number of clients to ensure sufficient resources are available. */
#define BA_MULTI_MAX_CLIENTS 32

/* Size of epoll event array. Allow for client control, pcm, and drain timer,
 * plus the mix event fd. */
#define BA_MULTI_MAX_EVENTS (1 + BA_MULTI_MAX_CLIENTS * 3)

/* Determines the size of the mix buffer. */
#define BA_MULTI_BUFFER_PERIODS 16

/* Internal period time */
#define BA_MULTI_PERIOD_MS 20

static void *ba_pcm_mix_thread_func(struct ba_pcm_multi *multi);
static void *ba_pcm_snoop_thread_func(struct ba_pcm_multi *multi);
static void ba_pcm_multi_remove_client(struct ba_pcm_multi *multi, struct ba_pcm_client *client);


static bool ba_pcm_multi_is_source(const struct ba_pcm_multi *multi) {
	return multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE;
}

static bool ba_pcm_multi_is_sink(const struct ba_pcm_multi *multi) {
	return multi->pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
}

static bool ba_pcm_multi_is_target(const struct ba_pcm_multi *multi) {
	return multi->pcm->t->profile & (BA_TRANSPORT_PROFILE_A2DP_SINK | BA_TRANSPORT_PROFILE_MASK_HF);
}

static void ba_pcm_multi_cleanup(struct ba_pcm_multi *multi) {
	if (multi->thread != config.main_thread) {
		eventfd_write(multi->event_fd, 0xDEAD0000);
		pthread_join(multi->thread, NULL);
		multi->thread = config.main_thread;
	}
	if (ba_pcm_multi_is_sink(multi) && multi->playback_buffer.size > 0)
		ba_pcm_mix_buffer_release(&multi->playback_buffer);

	pthread_mutex_lock(&multi->client_mutex);
	while (multi->client_count > 0)
		ba_pcm_multi_remove_client(multi, g_list_first(multi->clients)->data);
	multi->client_count = 0;
	pthread_mutex_unlock(&multi->client_mutex);
}

static void ba_pcm_multi_init_clients(struct ba_pcm_multi *multi) {
	pthread_mutex_lock(&multi->client_mutex);
	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct ba_pcm_client *client = el->data;
		if (client->buffer == NULL) {
			if (!ba_pcm_client_init(client))
				ba_pcm_multi_remove_client(client->multi, client);
		}
	}
	pthread_mutex_unlock(&multi->client_mutex);
}

static void ba_pcm_multi_underrun(struct ba_pcm_multi *multi) {
	pthread_mutex_lock(&multi->client_mutex);
	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct ba_pcm_client *client = el->data;
		ba_pcm_client_underrun(client);
	}
	pthread_mutex_unlock(&multi->client_mutex);
}

/**
 * Start the multi client thread. */
static bool ba_pcm_multi_start(struct ba_pcm_multi *multi) {

	if (ba_pcm_multi_is_sink(multi)) {
		if (pthread_create(&multi->thread, NULL, PTHREAD_FUNC(ba_pcm_mix_thread_func), multi) == -1) {
			error("Cannot create pcm multi mix thread: %s", strerror(errno));
			ba_pcm_mix_buffer_release(&multi->playback_buffer);
			multi->thread = config.main_thread;
			return false;
		}
		pthread_setname_np(multi->thread, "ba-pcm-mix");
	}
	else {
		if (pthread_create(&multi->thread, NULL, PTHREAD_FUNC(ba_pcm_snoop_thread_func), multi) == -1) {
			error("Cannot create pcm multi snoop thread: %s", strerror(errno));
			multi->thread = config.main_thread;
			return false;
		}
		pthread_setname_np(multi->thread, "ba-pcm-snoop");
	}

	return true;
}

/**
 * Is multi-client support implemented and configured for the given
 * transport pcm ? */
bool ba_pcm_multi_enabled(const struct ba_transport_pcm *pcm) {
	if (config.multi_mix_enabled && pcm->mode == BA_TRANSPORT_PCM_MODE_SINK) {
		if (pcm->t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
			return pcm->format != BA_TRANSPORT_PCM_FORMAT_S24_3LE;
		return true;
	}
	return config.multi_snoop_enabled && pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE;
}

/**
 * The current delay due to buffering within the multi.
 *
 * The BlueALSA API can return only a single value to all clients for each PCM,
 * so the value reported here is necessarily only an estimate, based on the
 * number of unread frames in the mix buffer plus a constant value
 * approximating the "typical" number of frames held in a client read buffer. */
int ba_pcm_multi_delay_get(const struct ba_pcm_multi *multi) {
	if (!ba_pcm_multi_is_sink(multi) || !multi->period_frames)
		return 0;
	int delay = ((ba_pcm_mix_buffer_delay(&multi->playback_buffer, multi->playback_buffer.end) / multi->pcm->channels)  + (BA_MULTI_CLIENT_THRESHOLD * multi->period_frames)) * 100 / multi->pcm->rate;
	return delay;
}

/**
 * Create multi-client support for the given transport pcm. */
struct ba_pcm_multi *ba_pcm_multi_create(struct ba_transport_pcm *pcm) {

	struct ba_pcm_multi *multi = calloc(1, sizeof(struct ba_pcm_multi));
	if (multi == NULL)
		return multi;

	multi->pcm = pcm;
	multi->thread = config.main_thread;

	pthread_mutex_init(&multi->client_mutex, NULL);
	pthread_mutex_init(&multi->buffer_mutex, NULL);
	pthread_cond_init(&multi->cond, NULL);

	if ((multi->epoll_fd = epoll_create(1)) == -1)
		goto fail;

	if ((multi->event_fd = eventfd(0, 0)) == -1)
		goto fail;

	pcm->multi = multi;

	return multi;

fail:
	if (multi->epoll_fd != -1)
		close(multi->epoll_fd);
	if (multi->event_fd != -1)
		close(multi->event_fd);
	free(multi);
	return NULL;
}

/**
 * Initialize multi-client support.
 *
 * Enable client audio I/O.
 *
 * @param multi The multi-client instance to be initialized.
 * @return true if multi-client successfully initialized. */
bool ba_pcm_multi_init(struct ba_pcm_multi *multi) {
	debug("Initializing multi client support");

	multi->state = BA_PCM_MULTI_STATE_INIT;
	multi->period_frames = BA_MULTI_PERIOD_MS * multi->pcm->rate / 1000;
	multi->period_bytes = multi->period_frames * multi->pcm->channels * BA_TRANSPORT_PCM_FORMAT_BYTES(multi->pcm->format);

	if (ba_pcm_multi_is_sink(multi)) {
		size_t buffer_frames = BA_MULTI_BUFFER_PERIODS * multi->period_frames;
		if (ba_pcm_mix_buffer_init(&multi->playback_buffer,
				multi->pcm->format, multi->pcm->channels,
				buffer_frames, multi->period_frames) == -1)
			return false;
		multi->buffer_ready = false;
		multi->active_count = 0;
	}

	multi->drain = false;
	multi->drop = false;

	ba_pcm_multi_init_clients(multi);

	if (ba_pcm_multi_is_source(multi) && multi->client_count > 0) {
		if (multi->thread == config.main_thread && !ba_pcm_multi_start(multi))
			return false;
	}
	return true;
}

/**
 * Stop the multi-client support. */
void ba_pcm_multi_reset(struct ba_pcm_multi *multi) {
	if (!ba_pcm_multi_is_target(multi))
		ba_pcm_multi_cleanup(multi);
	multi->state = BA_PCM_MULTI_STATE_INIT;
}

/**
 * Release the resources used by a multi. */
void ba_pcm_multi_free(struct ba_pcm_multi *multi) {
	ba_pcm_multi_cleanup(multi);
	g_list_free(multi->clients);

	close(multi->epoll_fd);
	close(multi->event_fd);

	pthread_mutex_destroy(&multi->client_mutex);
	pthread_mutex_destroy(&multi->buffer_mutex);
	pthread_cond_destroy(&multi->cond);

	free(multi);
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
bool ba_pcm_multi_add_client(struct ba_pcm_multi *multi, int pcm_fd, int control_fd) {
	int rv = 0;
	bool close_fd_on_fail = false;

	if (multi->client_count == BA_MULTI_MAX_CLIENTS)
		return false;

	if (ba_pcm_multi_is_source(multi) && multi->state == BA_PCM_MULTI_STATE_FINISHED) {
		/* client thread has failed - clean it up before starting new one. */
		ba_pcm_multi_reset(multi);
	}

	pthread_mutex_lock(&multi->pcm->mutex);
	if (multi->pcm->fd == -1) {
		rv = multi->pcm->fd = eventfd(0, EFD_NONBLOCK);
		close_fd_on_fail = true;
	}
	pthread_mutex_unlock(&multi->pcm->mutex);
	if (rv == -1)
		return false;

	struct ba_pcm_client *client = ba_pcm_client_new(multi, pcm_fd, control_fd);
	if (!client)
		goto fail;

	/* Postpone initialization of client if multi itself is not yet
	 * initialized. */
	if (multi->period_bytes > 0) {
		if (!ba_pcm_client_init(client)) {
			ba_pcm_client_free(client);
			goto fail;
		}
	}

#if DEBUG
	client->id = ++multi->client_no;
#endif

	pthread_mutex_lock(&multi->client_mutex);

	multi->clients = g_list_prepend(multi->clients, client);
	multi->client_count++;

	if (ba_pcm_multi_is_sink(multi)) {
		if (multi->state == BA_PCM_MULTI_STATE_FINISHED)
			multi->state = BA_PCM_MULTI_STATE_INIT;
	}
	else {
		if (multi->state == BA_PCM_MULTI_STATE_INIT)
			multi->state = BA_PCM_MULTI_STATE_RUNNING;
	}

	pthread_mutex_unlock(&multi->client_mutex);

	if (multi->thread == config.main_thread && !ba_pcm_multi_start(multi))
		goto fail;

	if (multi->client_count == 1)
		/* notify our PCM IO thread that the PCM was opened */
		ba_transport_pcm_signal_send(multi->pcm, BA_TRANSPORT_PCM_SIGNAL_OPEN);


	debug("new client id %zu, total clients now %zu", client->id, multi->client_count);
	return true;

fail:
	if (close_fd_on_fail) {
		pthread_mutex_lock(&multi->pcm->mutex);
		if (multi->pcm->fd != -1) {
			close(multi->pcm->fd);
			multi->pcm->fd = -1;
		}
		pthread_mutex_unlock(&multi->pcm->mutex);
	}
	return false;
}

/**
 * Remove a client stream.
 * @return false if no clients remain, true otherwise. */
static void ba_pcm_multi_remove_client(struct ba_pcm_multi *multi, struct ba_pcm_client *client) {
	client->multi->clients = g_list_remove(multi->clients, client);
	--client->multi->client_count;
	debug("removed client no %zu, total clients now %zu", client->id, multi->client_count);
	ba_pcm_client_free(client);
}

/**
 * Write out decoded samples to the clients.
 *
 * Called by the transport I/O thread.
 * @param multi Pointer to the multi.
 * @param buffer Pointer to the buffer from which to obtain the samples.
 * @param samples the number of samples available in the decoder buffer.
 * @return the number of samples written. */
ssize_t ba_pcm_multi_write(struct ba_pcm_multi *multi, const void *buffer, size_t samples) {

	pthread_mutex_lock(&multi->client_mutex);

	if (multi->state == BA_PCM_MULTI_STATE_FINISHED) {
		pthread_mutex_lock(&multi->pcm->mutex);
		ba_transport_pcm_release(multi->pcm);
		pthread_mutex_unlock(&multi->pcm->mutex);
		samples = 0;
		goto finish;
	}

	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct ba_pcm_client *client = el->data;
		enum ba_pcm_client_state client_state;
		pthread_mutex_lock(&client->mutex);
		client_state = client->state;
		pthread_mutex_unlock(&client->mutex);
		if (client_state == BA_PCM_CLIENT_STATE_RUNNING) {
			ba_pcm_client_write(client, buffer, samples);
			pthread_mutex_lock(&client->mutex);
			client_state = client->state;
			pthread_mutex_unlock(&client->mutex);
		}
		if (client_state == BA_PCM_CLIENT_STATE_FINISHED) {
			ba_pcm_multi_remove_client(multi, client);
		}
	}

finish:
	pthread_mutex_unlock(&multi->client_mutex);
	return (ssize_t) samples;
}

/**
 * Read mixed samples.
 *
 * multi client replacement for io_pcm_read() */
ssize_t ba_pcm_multi_read(struct ba_pcm_multi *multi, void *buffer, size_t samples) {
	eventfd_t value = 0;
	ssize_t ret;
	enum ba_pcm_multi_state state;

	pthread_mutex_lock(&multi->pcm->mutex);
	if (multi->pcm->fd == -1) {
		pthread_mutex_unlock(&multi->pcm->mutex);
		errno = EBADF;
		return -1;
	}

	/* Clear pcm available event */
	ret = eventfd_read(multi->pcm->fd, &value);
	pthread_mutex_unlock(&multi->pcm->mutex);
	if (ret < 0 && errno != EAGAIN)
		return ret;

	/* Trigger client thread to re-fill the mix. */
	pthread_mutex_lock(&multi->buffer_mutex);
	eventfd_write(multi->event_fd, 1);

	/* Wait for mix update to complete */
	while (((state = multi->state) == BA_PCM_MULTI_STATE_RUNNING) && !multi->buffer_ready)
		pthread_cond_wait(&multi->cond, &multi->buffer_mutex);
	multi->buffer_ready = false;
	pthread_mutex_unlock(&multi->buffer_mutex);

	switch (state) {
	case BA_PCM_MULTI_STATE_RUNNING:
	{
		double scale_array[multi->pcm->channels];
		if (multi->pcm->soft_volume) {
			for (unsigned i = 0; i < multi->pcm->channels; ++i)
				scale_array[i] = multi->pcm->volume[i].scale;
		}
		else {
			for (unsigned i = 0; i < multi->pcm->channels; ++i)
				/* For pass-through volume control, we silence the samples if
				 * mute is enabled, otherwise we apply the configured mix
				 * attenuation. */
				if (multi->pcm->volume[i].scale == 0)
					scale_array[i] = 0;
				else
					scale_array[i] = config.multi_native_volume;
		}
		pthread_mutex_lock(&multi->buffer_mutex);
		ret = ba_pcm_mix_buffer_read(&multi->playback_buffer, buffer, samples, scale_array);
		pthread_mutex_unlock(&multi->buffer_mutex);
		if (ret == 0) {
			/* The mix buffer is empty. Any clients still running must have
			 * underrun. */
			ba_pcm_multi_underrun(multi);
			errno = EAGAIN;
			ret = -1;
		}
		break;
	}
	case BA_PCM_MULTI_STATE_FINISHED:
		pthread_mutex_lock(&multi->pcm->mutex);
		ba_transport_pcm_release(multi->pcm);
		pthread_mutex_unlock(&multi->pcm->mutex);
		ret = 0;
		break;
	case BA_PCM_MULTI_STATE_INIT:
		errno = EAGAIN;
		ret = -1;
		break;
	default:
		errno = EIO;
		ret = -1;
		break;
	}

	return ret;
}

/**
 * Signal the transport i/o thread that mixed samples are available. */
static void ba_pcm_multi_wake_transport(struct ba_pcm_multi *multi) {
	pthread_mutex_lock(&multi->pcm->mutex);
	eventfd_write(multi->pcm->fd, 1);
	pthread_mutex_unlock(&multi->pcm->mutex);
}

/**
 * Add more samples from clients into the mix.
 * Caller must hold lock on multi client_mutex  */
static void ba_pcm_multi_update_mix(struct ba_pcm_multi *multi) {
	GList *el;
	for (el = multi->clients; el != NULL; el = el->next) {
		struct ba_pcm_client *client = el->data;
		ba_pcm_client_deliver(client);
	}
}

/**
 * Inform the io thread that the last client has closed its connection. */
static void ba_pcm_multi_close(struct ba_pcm_multi *multi) {
	pthread_mutex_lock(&multi->pcm->mutex);
	ba_transport_pcm_release(multi->pcm);
	pthread_mutex_unlock(&multi->pcm->mutex);
	ba_transport_pcm_signal_send(multi->pcm, BA_TRANSPORT_PCM_SIGNAL_CLOSE);
}

/**
 * The mix thread. */
static void *ba_pcm_mix_thread_func(struct ba_pcm_multi *multi) {

	struct epoll_event events[BA_MULTI_MAX_EVENTS] = { 0 };

	struct epoll_event event = {
		.events =  EPOLLIN,
		.data.ptr = multi,
	};

	epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, multi->event_fd, &event);

	debug("Starting pcm mix loop");
	for (;;) {

		int event_count;
		do {
			event_count = epoll_wait(multi->epoll_fd, events, BA_MULTI_MAX_EVENTS, -1);
		} while (event_count == -1 && errno == EINTR);

		if (event_count <= 0) {
			error("epoll_wait failed: %d (%s)", errno, strerror(errno));
			goto terminate;
		}

		for (int n = 0; n < event_count; n++) {

			if (events[n].data.ptr == multi) {
				/* trigger from encoder thread */
				eventfd_t value = 0;
				eventfd_read(multi->event_fd, &value);
				if (value >= 0xDEAD0000)
					goto terminate;
				pthread_mutex_lock(&multi->buffer_mutex);
				pthread_mutex_lock(&multi->client_mutex);
				ba_pcm_multi_update_mix(multi);
				pthread_mutex_unlock(&multi->client_mutex);
				multi->buffer_ready = true;
				pthread_mutex_unlock(&multi->buffer_mutex);
				pthread_cond_signal(&multi->cond);
				break;
			}

			else {   /* client event */
				struct ba_pcm_client_event *cevent = events[n].data.ptr;
				struct ba_pcm_client *client = cevent->client;

				ba_pcm_client_handle_event(cevent);

				if (client->state == BA_PCM_CLIENT_STATE_FINISHED) {
					pthread_mutex_lock(&multi->client_mutex);
					ba_pcm_multi_remove_client(multi, client);
					pthread_mutex_unlock(&multi->client_mutex);

					/* removing a client invalidates the event array, so
					 * we need to call epoll_wait() again here */
					break;
				}
			}
		}

		if (multi->client_count == 0) {
			multi->state = BA_PCM_MULTI_STATE_FINISHED;
			pthread_mutex_lock(&multi->buffer_mutex);
			ba_pcm_mix_buffer_clear(&multi->playback_buffer);
			pthread_mutex_unlock(&multi->buffer_mutex);
			ba_pcm_multi_close(multi);
			continue;
		}

		if (multi->client_count == 1) {
			struct ba_pcm_client* client = g_list_first(multi->clients)->data;
			if (client->drop) {
				pthread_mutex_lock(&multi->pcm->mutex);
				pthread_mutex_lock(&multi->buffer_mutex);
				ba_pcm_mix_buffer_clear(&multi->playback_buffer);
				pthread_mutex_unlock(&multi->buffer_mutex);
				/* Clear any remaining pcm available event */
				if (multi->pcm->fd != -1) {
					eventfd_t value;
					eventfd_read(multi->pcm->fd, &value);
				}
				pthread_mutex_unlock(&multi->pcm->mutex);
				ba_transport_pcm_drop(multi->pcm);
				client->drop = false;
				multi->state = BA_PCM_MULTI_STATE_INIT;
				continue;
			}
		}

		if (multi->state == BA_PCM_MULTI_STATE_INIT) {
			if (multi->active_count > 0) {
				pthread_mutex_lock(&multi->buffer_mutex);
				pthread_mutex_lock(&multi->client_mutex);
				ba_pcm_multi_update_mix(multi);
				pthread_mutex_unlock(&multi->client_mutex);
				if (ba_pcm_mix_buffer_at_threshold(&multi->playback_buffer)) {
					multi->state = BA_PCM_MULTI_STATE_RUNNING;
					ba_transport_pcm_resume(multi->pcm);
				}
				pthread_mutex_unlock(&multi->buffer_mutex);
			}
		}
		else if (multi->state == BA_PCM_MULTI_STATE_RUNNING) {
			if (ba_pcm_mix_buffer_empty(&multi->playback_buffer))
				multi->state = BA_PCM_MULTI_STATE_INIT;
			else
				ba_pcm_multi_wake_transport(multi);
		}
	}

terminate:
	multi->state = BA_PCM_MULTI_STATE_FINISHED;
	pthread_cond_signal(&multi->cond);
	ba_pcm_multi_wake_transport(multi);
	debug("mix thread function terminated");
	return NULL;
}


/**
 * The snoop thread. */
static void *ba_pcm_snoop_thread_func(struct ba_pcm_multi *multi) {

	struct epoll_event events[BA_MULTI_MAX_EVENTS];

	struct epoll_event event = {
		.events =  EPOLLIN,
		.data.ptr = multi,
	};

	epoll_ctl(multi->epoll_fd, EPOLL_CTL_ADD, multi->event_fd, &event);

	debug("Starting pcm snoop loop");
	for (;;) {
		int ret;

		do {
			ret = epoll_wait(multi->epoll_fd, events, BA_MULTI_MAX_EVENTS, -1);
		} while (ret == -1 && errno == EINTR);

		if (ret <= 0) {
			error("epoll_wait failed: %d (%s)", errno, strerror(errno));
			goto terminate;
		}

		for (int n = 0; n < ret; n++) {

			if (events[n].data.ptr == multi) {
				/* trigger from transport thread */

				eventfd_t value = 0;
				eventfd_read(multi->event_fd, &value);
				if (value >= 0xDEAD0000)
					goto terminate;

			}

			else {
				/* client event */
				struct ba_pcm_client_event *cevent = events[n].data.ptr;
				if (events[n].events & (EPOLLHUP|EPOLLERR)) {
					ba_pcm_client_handle_close_event(cevent);
					pthread_mutex_lock(&multi->client_mutex);
					ba_pcm_multi_remove_client(multi, cevent->client);
					pthread_mutex_unlock(&multi->client_mutex);
					if (multi->client_count == 0) {
						multi->state = BA_PCM_MULTI_STATE_FINISHED;
						ba_pcm_multi_close(multi);
					}

					/* removing a client invalidates the event array, so
					 * we need to call epoll_wait() again here */
					break;
				}
				else {
					ba_pcm_client_handle_event(cevent);
					if (multi->state == BA_PCM_MULTI_STATE_PAUSED && multi->active_count > 0) {
						multi->state = BA_PCM_MULTI_STATE_RUNNING;
						ba_transport_pcm_resume(multi->pcm);
;
					}
				}
			}
		}
	}

terminate:
	multi->state = BA_PCM_MULTI_STATE_FINISHED;
	debug("snoop thread function terminated");
	return NULL;
}
