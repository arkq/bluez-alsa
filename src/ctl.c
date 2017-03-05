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
	gpointer _tmp;

	for (g_hash_table_iter_init(&iter_d, devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)&d); ) {

		if (bacmp(&d->addr, addr) != 0)
			continue;

		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, &_tmp, (gpointer)t); ) {

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
				if ((*t)->sco.codec == TRANSPORT_SCO_CODEC_UNKNOWN)
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

static void ctl_thread_cmd_list_devices(const struct request *req, int fd) {
	(void)req;

	static const struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_device device;
	GHashTableIter iter_d;
	struct ba_device *d;
	gpointer _tmp;

	pthread_mutex_lock(&config.devices_mutex);

	for (g_hash_table_iter_init(&iter_d, config.devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)&d); ) {

		bacpy(&device.addr, &d->addr);
		strncpy(device.name, d->name, sizeof(device.name) - 1);
		device.name[sizeof(device.name) - 1] = '\0';

		device.battery = d->xapl.features & DEVICE_XAPL_FEATURE_BATTERY ? 1 : 0;
		device.battery_level = d->xapl.accev_battery;

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
	gpointer _tmp;

	pthread_mutex_lock(&config.devices_mutex);

	for (g_hash_table_iter_init(&iter_d, config.devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)&d); )
		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, &_tmp, (gpointer)&t); ) {
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
	while (config.ctl_thread_created) {

		if (poll(config.ctl_pfds, 1 + BLUEALSA_MAX_CLIENTS, -1) == -1) {
			error("Controller poll error: %s", strerror(errno));
			break;
		}

		/* Clients handling loop will update this variable to point to the
		 * first available client structure, which might be later used by
		 * the connection handling loop. */
		struct pollfd *pfd = NULL;
		size_t i;

		/* handle data transmission with connected clients */
		for (i = 1; i < 1 + BLUEALSA_MAX_CLIENTS; i++) {
			const int fd = config.ctl_pfds[i].fd;

			if (fd == -1) {
				pfd = &config.ctl_pfds[i];
				continue;
			}

			if (config.ctl_pfds[i].revents & POLLIN) {

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

					config.ctl_pfds[i].fd = -1;
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
		if (config.ctl_pfds[0].revents & POLLIN && pfd != NULL) {
			pfd->fd = accept(config.ctl_pfds[0].fd, NULL, NULL);
			debug("New client accepted: %d", pfd->fd);
		}

		debug("+-+-");
	}

	debug("Exiting controller thread");
	return NULL;
}

int bluealsa_ctl_thread_init(void) {

	if (config.ctl_thread_created) {
		/* thread is already created */
		errno = EISCONN;
		return -1;
	}

	{ /* initialize (mark as closed) all sockets */
		size_t i;
		for (i = 0; i < 1 + BLUEALSA_MAX_CLIENTS; i++) {
			config.ctl_pfds[i].events = POLLIN;
			config.ctl_pfds[i].fd = -1;
		}
	}

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", config.hci_dev.name);

	if (mkdir(BLUEALSA_RUN_STATE_DIR, 0755) == -1 && errno != EEXIST)
		goto fail;
	if ((config.ctl_pfds[0].fd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1)
		goto fail;
	if (bind(config.ctl_pfds[0].fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1)
		goto fail;
	config.ctl_socket_created = 1;
	if (chmod(saddr.sun_path, 0660) == -1)
		goto fail;
	if (chown(saddr.sun_path, -1, config.gid_audio) == -1)
		goto fail;
	if (listen(config.ctl_pfds[0].fd, 2) == -1)
		goto fail;

	config.ctl_thread_created = 1;
	if ((errno = pthread_create(&config.ctl_thread, NULL, ctl_thread, NULL)) != 0) {
		config.ctl_thread_created = 0;
		goto fail;
	}

	/* name controller thread - for aesthetic purposes only */
	pthread_setname_np(config.ctl_thread, "bactl");

	return 0;

fail:
	bluealsa_ctl_free();
	return -1;
}

void bluealsa_ctl_free(void) {

	int created = config.ctl_thread_created;
	size_t i;

	config.ctl_thread_created = 0;

	for (i = 0; i < 1 + BLUEALSA_MAX_CLIENTS; i++)
		if (config.ctl_pfds[i].fd != -1)
			close(config.ctl_pfds[i].fd);

	if (created) {
		pthread_cancel(config.ctl_thread);
		if ((errno = pthread_join(config.ctl_thread, NULL)) != 0)
			error("Couldn't join controller thread: %s", strerror(errno));
	}

	if (config.ctl_socket_created) {
		char tmp[256] = BLUEALSA_RUN_STATE_DIR "/";
		unlink(strcat(tmp, config.hci_dev.name));
		config.ctl_socket_created = 0;
	}

}
