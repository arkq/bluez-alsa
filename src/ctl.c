/*
 * BlueALSA - ctl.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include "ctl.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <glib.h>

#include "a2dp-codecs.h"
#include "bluealsa.h"
#include "bluez.h"
#include "hfp.h"
#include "transport.h"
#include "utils.h"
#include "shared/ctl-proto.h"
#include "shared/log.h"


/**
 * Looks up a transport matching BT address and profile.
 *
 * This function is not thread-safe. It returns references to objects managed
 * by the devices hash-table. If the devices hash-table is modified in some
 * other thread, it may result in an undefined behavior.
 *
 * @param devices Address of the hash-table with connected devices.
 * @param addr Address to the structure with the looked up BT address.
 * @param type Looked up PCM type.
 * @param stream Looked up PCM stream direction.
 * @param t Address, where the transport structure pointer should be stored.
 * @return If the lookup succeeded, this function returns 0. Otherwise, -1 is
 *   returned and value of transport pointer is undefined. */
static int _transport_lookup(GHashTable *devices, const bdaddr_t *addr,
		enum pcm_type type, enum pcm_stream stream, struct ba_transport **t) {

	GHashTableIter iter_d, iter_t;
	struct ba_device *d;

	for (g_hash_table_iter_init(&iter_d, devices);
			g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d); ) {

		if (bacmp(&d->addr, addr) != 0)
			continue;

		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, NULL, (gpointer)t); ) {

			switch (type) {
			case PCM_TYPE_NULL:
				continue;
			case PCM_TYPE_A2DP:
				if ((*t)->type != TRANSPORT_TYPE_A2DP)
					continue;
				switch (stream) {
				case PCM_STREAM_PLAYBACK:
					if ((*t)->profile != BLUETOOTH_PROFILE_A2DP_SOURCE)
						continue;
					break;
				case PCM_STREAM_CAPTURE:
					if ((*t)->profile != BLUETOOTH_PROFILE_A2DP_SINK)
						continue;
					break;
				case PCM_STREAM_DUPLEX:
					continue;
				}
				break;
			case PCM_TYPE_SCO:
				if ((*t)->type != TRANSPORT_TYPE_SCO)
					continue;
				/* ignore SCO transport if codec is not selected yet */
				if ((*t)->codec == HFP_CODEC_UNDEFINED)
					continue;
				break;
			}

			return 0;
		}

	}

	return -1;
}

/**
 * Get transport PCM structure.
 *
 * @param t Pointer to the transport structure.
 * @param stream Stream type.
 * @return On success address of the PCM structure is returned. If the PCM
 *   structure can not be determined, NULL is returned. */
static struct ba_pcm *_transport_get_pcm(struct ba_transport *t, enum pcm_stream stream) {
	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		return &t->a2dp.pcm;
	case TRANSPORT_TYPE_RFCOMM:
		debug("Trying to get PCM for RFCOMM transport... that's nuts");
		break;
	case TRANSPORT_TYPE_SCO:
		switch (stream) {
		case PCM_STREAM_PLAYBACK:
			return &t->sco.spk_pcm;
		case PCM_STREAM_CAPTURE:
			return &t->sco.mic_pcm;
		case PCM_STREAM_DUPLEX:
			break;
		}
	}
	return NULL;
}

/**
 * Release transport resources acquired by the controller module. */
static void _transport_release(struct ba_transport *t, int client) {

	/* For a source profile (where the stream is read from the PCM) an IO thread
	 * terminates when the PCM is closed. However, it is asynchronous, so if the
	 * client closes the connection, and then quickly tries to open it again, we
	 * might try to acquire not yet released transport. To prevent this, we have
	 * to make sure, that the transport is released (thread is terminated). */
	if (t->profile == BLUETOOTH_PROFILE_A2DP_SOURCE) {
		pthread_cancel(t->thread);
		pthread_join(t->thread, NULL);
	}

	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		transport_release_pcm(&t->a2dp.pcm);
		t->a2dp.pcm.client = -1;
		break;
	case TRANSPORT_TYPE_RFCOMM:
		break;
	case TRANSPORT_TYPE_SCO:
		if (t->sco.spk_pcm.client == client) {
			transport_release_pcm(&t->sco.spk_pcm);
			t->sco.spk_pcm.client = -1;
		}
		if (t->sco.mic_pcm.client == client) {
			transport_release_pcm(&t->sco.mic_pcm);
			t->sco.mic_pcm.client = -1;
		}
	}

}

static void _ctl_transport(const struct ba_transport *t, struct msg_transport *transport) {

	bacpy(&transport->addr, &t->device->addr);

	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		transport->type = PCM_TYPE_A2DP;
		transport->stream = t->profile == BLUETOOTH_PROFILE_A2DP_SOURCE ?
			PCM_STREAM_PLAYBACK : PCM_STREAM_CAPTURE;
		transport->ch1_muted = t->a2dp.ch1_muted;
		transport->ch1_volume = t->a2dp.ch1_volume;
		transport->ch2_muted = t->a2dp.ch2_muted;
		transport->ch2_volume = t->a2dp.ch2_volume;
		transport->delay = t->a2dp.delay;
		break;
	case TRANSPORT_TYPE_RFCOMM:
		transport->type = PCM_TYPE_NULL;
		break;
	case TRANSPORT_TYPE_SCO:
		transport->type = PCM_TYPE_SCO;
		transport->stream = PCM_STREAM_DUPLEX;
		transport->ch1_muted = t->sco.spk_muted;
		transport->ch1_volume = t->sco.spk_gain;
		transport->ch2_muted = t->sco.mic_muted;
		transport->ch2_volume = t->sco.mic_gain;
		transport->delay = 10;
		break;
	}

	transport->codec = t->codec;
	transport->channels = transport_get_channels(t);
	transport->sampling = transport_get_sampling(t);
	transport->delay += t->delay;

}

static void ctl_thread_cmd_ping(const struct request *req, int fd) {
	(void)req;
	static const struct msg_status status = { STATUS_CODE_PONG };
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_notifications(const struct request *req, int fd) {

	static const struct msg_status status = { STATUS_CODE_SUCCESS };
	const bool subscribe = req->command == COMMAND_SUBSCRIBE;
	size_t i;

	for (i = __CTL_IDX_MAX; i < __CTL_IDX_MAX + BLUEALSA_MAX_CLIENTS; i++)
		if (config.ctl.pfds[i].fd == fd)
			config.ctl.subs[i - __CTL_IDX_MAX] = subscribe;

	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_list_devices(const struct request *req, int fd) {
	(void)req;

	static const struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_device device;
	GHashTableIter iter_d;
	struct ba_device *d;

	pthread_mutex_lock(&config.devices_mutex);

	for (g_hash_table_iter_init(&iter_d, config.devices);
			g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d); ) {

		bacpy(&device.addr, &d->addr);
		strncpy(device.name, d->name, sizeof(device.name) - 1);
		device.name[sizeof(device.name) - 1] = '\0';

		device.battery = d->battery.enabled;
		device.battery_level = d->battery.level;

		send(fd, &device, sizeof(device), MSG_NOSIGNAL);
	}

	pthread_mutex_unlock(&config.devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_list_transports(const struct request *req, int fd) {
	(void)req;

	static const struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_transport transport;
	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t;

	pthread_mutex_lock(&config.devices_mutex);

	for (g_hash_table_iter_init(&iter_d, config.devices);
			g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d); )
		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, NULL, (gpointer)&t); ) {
			/* ignore SCO transport if codec is not selected yet */
			if (t->type == TRANSPORT_TYPE_SCO && t->codec == HFP_CODEC_UNDEFINED)
				continue;
			_ctl_transport(t, &transport);
			send(fd, &transport, sizeof(transport), MSG_NOSIGNAL);
		}

	pthread_mutex_unlock(&config.devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_transport_get(const struct request *req, int fd) {

	struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_transport transport;
	struct ba_transport *t;

	pthread_mutex_lock(&config.devices_mutex);

	if (_transport_lookup(config.devices, &req->addr, req->type, req->stream, &t) != 0) {
		status.code = STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	}

	_ctl_transport(t, &transport);
	send(fd, &transport, sizeof(transport), MSG_NOSIGNAL);

fail:
	pthread_mutex_unlock(&config.devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_transport_set_volume(const struct request *req, int fd) {

	struct msg_status status = { STATUS_CODE_SUCCESS };
	struct ba_transport *t;

	pthread_mutex_lock(&config.devices_mutex);

	if (_transport_lookup(config.devices, &req->addr, req->type, req->stream, &t) != 0) {
		status.code = STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	}

	transport_set_volume(t, req->ch1_muted, req->ch2_muted, req->ch1_volume, req->ch2_volume);

fail:
	pthread_mutex_unlock(&config.devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_pcm_open(const struct request *req, int fd) {

	struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_pcm pcm;
	struct ba_transport *t;
	struct ba_pcm *t_pcm;
	char addr[18];

	ba2str(&req->addr, addr);
	snprintf(pcm.fifo, sizeof(pcm.fifo), BLUEALSA_RUN_STATE_DIR "/%s-%s-%u-%u",
			config.hci_dev.name, addr, req->type, req->stream);
	pcm.fifo[sizeof(pcm.fifo) - 1] = '\0';

	debug("PCM requested for %s type %d stream %d", addr, req->type, req->stream);

	pthread_mutex_lock(&config.devices_mutex);

	if (_transport_lookup(config.devices, &req->addr, req->type, req->stream, &t) != 0) {
		status.code = STATUS_CODE_DEVICE_NOT_FOUND;
		goto final;
	}

	if ((t_pcm = _transport_get_pcm(t, req->stream)) == NULL) {
		status.code = STATUS_CODE_ERROR_UNKNOWN;
		goto final;
	}

	if (t_pcm->fifo != NULL) {
		debug("PCM already requested: %d", t_pcm->fd);
		status.code = STATUS_CODE_DEVICE_BUSY;
		goto final;
	}

	_ctl_transport(t, &pcm.transport);

	if (mkfifo(pcm.fifo, 0660) != 0) {
		error("Couldn't create FIFO: %s", strerror(errno));
		status.code = STATUS_CODE_ERROR_UNKNOWN;
		/* Jumping to the final section will prevent from unintentional removal
		 * of the FIFO, which was not created by ourself. Cleanup action should
		 * be applied to the stuff created in this function only. */
		goto final;
	}

	/* During the mkfifo() call the FIFO mode is modified by the process umask,
	 * so the post-creation correction is required. */
	if (chmod(pcm.fifo, 0660) == -1)
		goto fail;
	if (chown(pcm.fifo, -1, config.gid_audio) == -1)
		goto fail;

	/* XXX: This change will notify our sink IO thread, that the FIFO has just
	 *      been created, so it is possible to open it. Source IO thread should
	 *      not be started before the PCM open request has been made, so this
	 *      "notification" mechanism does not apply. */
	t_pcm->fifo = strdup(pcm.fifo);
	eventfd_write(t->event_fd, 1);

	/* A2DP source profile should be initialized (acquired) only if the audio
	 * is about to be transfered. It is most likely, that BT headset will not
	 * run voltage converter (power-on its circuit board) until the transport
	 * is acquired - in order to extend battery life. */
	if (t->profile == BLUETOOTH_PROFILE_A2DP_SOURCE)
		if (transport_acquire_bt_a2dp(t) == -1) {
			status.code = STATUS_CODE_ERROR_UNKNOWN;
			goto fail;
		}

	t_pcm->client = fd;
	send(fd, &pcm, sizeof(pcm), MSG_NOSIGNAL);
	goto final;

fail:
	free(t_pcm->fifo);
	t_pcm->fifo = NULL;
	unlink(pcm.fifo);

final:
	pthread_mutex_unlock(&config.devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_pcm_close(const struct request *req, int fd) {

	struct msg_status status = { STATUS_CODE_SUCCESS };
	struct ba_transport *t;
	struct ba_pcm *t_pcm;

	debug("PCM close for %s type %d stream %d", batostr_(&req->addr), req->type, req->stream);

	pthread_mutex_lock(&config.devices_mutex);

	if (_transport_lookup(config.devices, &req->addr, req->type, req->stream, &t) != 0) {
		status.code = STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	}
	if ((t_pcm = _transport_get_pcm(t, req->stream)) == NULL) {
		status.code = STATUS_CODE_ERROR_UNKNOWN;
		goto fail;
	}
	if (t_pcm->client != fd) {
		status.code = STATUS_CODE_FORBIDDEN;
		goto fail;
	}

	_transport_release(t, fd);
	eventfd_write(t->event_fd, 1);

fail:
	pthread_mutex_unlock(&config.devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_pcm_control(const struct request *req, int fd) {

	struct msg_status status = { STATUS_CODE_SUCCESS };
	struct ba_transport *t;
	struct ba_pcm *t_pcm;

	pthread_mutex_lock(&config.devices_mutex);

	if (_transport_lookup(config.devices, &req->addr, req->type, req->stream, &t) != 0) {
		status.code = STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	}
	if ((t_pcm = _transport_get_pcm(t, req->stream)) == NULL) {
		status.code = STATUS_CODE_ERROR_UNKNOWN;
		goto fail;
	}
	if (t_pcm->fifo == NULL || t_pcm->client == -1) {
		status.code = STATUS_CODE_ERROR_UNKNOWN;
		goto fail;
	}
	if (t_pcm->client != fd) {
		status.code = STATUS_CODE_FORBIDDEN;
		goto fail;
	}

	switch (req->command) {
	case COMMAND_PCM_PAUSE:
		transport_set_state(t, TRANSPORT_PAUSED);
		break;
	case COMMAND_PCM_RESUME:
		transport_set_state(t, TRANSPORT_ACTIVE);
		break;
	case COMMAND_PCM_READY:
		break;
	default:
		warn("Invalid PCM control command: %d", req->command);
	}

	eventfd_write(t->event_fd, 1);

fail:
	pthread_mutex_unlock(&config.devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void *ctl_thread(void *arg) {
	(void)arg;

	static void (*commands[__COMMAND_MAX])(const struct request *, int) = {
		[COMMAND_PING] = ctl_thread_cmd_ping,
		[COMMAND_SUBSCRIBE] = ctl_thread_cmd_notifications,
		[COMMAND_UNSUBSCRIBE] = ctl_thread_cmd_notifications,
		[COMMAND_LIST_DEVICES] = ctl_thread_cmd_list_devices,
		[COMMAND_LIST_TRANSPORTS] = ctl_thread_cmd_list_transports,
		[COMMAND_TRANSPORT_GET] = ctl_thread_cmd_transport_get,
		[COMMAND_TRANSPORT_SET_VOLUME] = ctl_thread_cmd_transport_set_volume,
		[COMMAND_PCM_OPEN] = ctl_thread_cmd_pcm_open,
		[COMMAND_PCM_CLOSE] = ctl_thread_cmd_pcm_close,
		[COMMAND_PCM_PAUSE] = ctl_thread_cmd_pcm_control,
		[COMMAND_PCM_RESUME] = ctl_thread_cmd_pcm_control,
		[COMMAND_PCM_READY] = ctl_thread_cmd_pcm_control,
	};

	debug("Starting controller loop");
	while (config.ctl.thread_created) {

		if (poll(config.ctl.pfds, sizeof(config.ctl.pfds) / sizeof(*config.ctl.pfds), -1) == -1) {
			error("Controller poll error: %s", strerror(errno));
			break;
		}

		/* Clients handling loop will update this variable to point to the
		 * first available client structure, which might be later used by
		 * the connection handling loop. */
		struct pollfd *pfd = NULL;
		size_t i;

		/* handle data transmission with connected clients */
		for (i = __CTL_IDX_MAX; i < __CTL_IDX_MAX + BLUEALSA_MAX_CLIENTS; i++) {
			const int fd = config.ctl.pfds[i].fd;

			if (fd == -1) {
				pfd = &config.ctl.pfds[i];
				continue;
			}

			if (config.ctl.pfds[i].revents & POLLIN) {

				struct request request;
				ssize_t len;

				if ((len = recv(fd, &request, sizeof(request), MSG_DONTWAIT)) != sizeof(request)) {
					/* if the request cannot be retrieved, release resources */

					if (len == 0)
						debug("Client closed connection: %d", fd);
					else
						debug("Invalid request length: %zd != %zd", len, sizeof(request));

					struct ba_transport *t;
					if ((t = transport_lookup_pcm_client(config.devices, fd)) != NULL) {
						_transport_release(t, fd);
						eventfd_write(t->event_fd, 1);
					}

					config.ctl.pfds[i].fd = -1;
					config.ctl.subs[i - __CTL_IDX_MAX] = false;
					close(fd);
					continue;
				}

				/* validate and execute requested command */
				if (request.command < __COMMAND_MAX && commands[request.command] != NULL)
					commands[request.command](&request, fd);
				else
					warn("Invalid command: %u", request.command);

			}

		}

		/* process new connections to our controller */
		if (config.ctl.pfds[CTL_IDX_SRV].revents & POLLIN && pfd != NULL) {
			pfd->fd = accept(config.ctl.pfds[CTL_IDX_SRV].fd, NULL, NULL);
			debug("New client accepted: %d", pfd->fd);
		}

		/* generate notifications for subscribed clients */
		if (config.ctl.pfds[CTL_IDX_EVT].revents & POLLIN) {

			struct msg_event event = { EVENT_TYPE_NULL };
			eventfd_t _event;
			size_t i;

			eventfd_read(config.ctl.pfds[CTL_IDX_EVT].fd, &_event);

			for (i = 0; i < BLUEALSA_MAX_CLIENTS; i++)
				if (config.ctl.subs[i] == true) {
					const int client = config.ctl.pfds[i + __CTL_IDX_MAX].fd;
					debug("Generating client notification: %d", client);
					send(client, &event, sizeof(event), MSG_NOSIGNAL);
				}

		}

		debug("+-+-");
	}

	debug("Exiting controller thread");
	return NULL;
}

int bluealsa_ctl_thread_init(void) {

	if (config.ctl.thread_created) {
		/* thread is already created */
		errno = EISCONN;
		return -1;
	}

	{ /* initialize (mark as closed) all sockets */
		size_t i;
		for (i = 0; i < sizeof(config.ctl.pfds) / sizeof(*config.ctl.pfds); i++) {
			config.ctl.pfds[i].events = POLLIN;
			config.ctl.pfds[i].fd = -1;
		}
	}

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", config.hci_dev.name);

	if (mkdir(BLUEALSA_RUN_STATE_DIR, 0755) == -1 && errno != EEXIST)
		goto fail;
	if ((config.ctl.pfds[CTL_IDX_SRV].fd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1)
		goto fail;
	if (bind(config.ctl.pfds[CTL_IDX_SRV].fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1)
		goto fail;
	config.ctl.socket_created = true;
	if (chmod(saddr.sun_path, 0660) == -1)
		goto fail;
	if (chown(saddr.sun_path, -1, config.gid_audio) == -1)
		goto fail;
	if (listen(config.ctl.pfds[CTL_IDX_SRV].fd, 2) == -1)
		goto fail;

	if ((config.ctl.pfds[CTL_IDX_EVT].fd = eventfd(0, EFD_NONBLOCK)) == -1)
		goto fail;

	config.ctl.thread_created = true;
	if ((errno = pthread_create(&config.ctl.thread, NULL, ctl_thread, NULL)) != 0) {
		config.ctl.thread_created = false;
		goto fail;
	}

	/* name controller thread - for aesthetic purposes only */
	pthread_setname_np(config.ctl.thread, "bactl");

	return 0;

fail:
	bluealsa_ctl_free();
	return -1;
}

void bluealsa_ctl_free(void) {

	int created = config.ctl.thread_created;
	size_t i;

	config.ctl.thread_created = false;

	for (i = 0; i < sizeof(config.ctl.pfds) / sizeof(*config.ctl.pfds); i++)
		if (config.ctl.pfds[i].fd != -1)
			close(config.ctl.pfds[i].fd);

	if (created) {
		pthread_cancel(config.ctl.thread);
		if ((errno = pthread_join(config.ctl.thread, NULL)) != 0)
			error("Couldn't join controller thread: %s", strerror(errno));
	}

	if (config.ctl.socket_created) {
		char tmp[256] = BLUEALSA_RUN_STATE_DIR "/";
		unlink(strcat(tmp, config.hci_dev.name));
		config.ctl.socket_created = false;
	}

}
