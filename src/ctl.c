/*
 * BlueALSA - ctl.c
 * Copyright (c) 2016 Arkadiusz Bokowy
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <glib.h>

#include "a2dp-codecs.h"
#include "bluealsa.h"
#include "bluez.h"
#include "log.h"
#include "transport.h"
#include "utils.h"


/**
 * Looks up a transport matching BT address and profile.
 *
 * This function is not thread-safe. It returns references to objects managed
 * by the devices hash-table. If the devices hash-table is modified in some
 * other thread, it may result in an undefined behavior.
 *
 * @param devices Address of the hash-table with connected devices.
 * @param addr Address to the structure with the looked up BT address.
 * @param profile Looked up transport profile.
 * @param d Address, where the device structure pointer should be stored.
 * @param t Address, where the transport structure pointer should be stored.
 * @return If the lookup succeeded, this function returns 0. Otherwise, -1 is
 *   returned and value of device and transport pointer is undefined. */
static int _transport_lookup(GHashTable *devices, const bdaddr_t *addr, uint8_t profile,
		struct ba_device **d, struct ba_transport **t) {

	GHashTableIter iter_d, iter_t;
	gpointer _tmp;

	for (g_hash_table_iter_init(&iter_d, devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)d); ) {

		if (bacmp(&(*d)->addr, addr) != 0)
			continue;

		for (g_hash_table_iter_init(&iter_t, (*d)->transports);
				g_hash_table_iter_next(&iter_t, &_tmp, (gpointer)t); )
			if ((*t)->profile == profile)
				return 0;

	}

	return -1;
}

static void _ctl_transport(const struct ba_device *d, const struct ba_transport *t,
		struct msg_transport *transport) {

	bacpy(&transport->addr, &d->addr);

	strncpy(transport->name, d->name, sizeof(transport->name) - 1);
	transport->name[sizeof(transport->name) - 1] = '\0';

	transport->profile = t->profile;
	transport->codec = t->codec;

	transport->volume = t->volume;
	transport->muted = t->muted;

	/* TODO: support other profiles and codecs */

	a2dp_sbc_t *c;
	c = (a2dp_sbc_t *)t->config;

	switch (c->channel_mode) {
	case SBC_CHANNEL_MODE_MONO:
		transport->channels = 1;
		break;
	case SBC_CHANNEL_MODE_STEREO:
	case SBC_CHANNEL_MODE_JOINT_STEREO:
	case SBC_CHANNEL_MODE_DUAL_CHANNEL:
		transport->channels = 2;
		break;
	default:
		transport->channels = 0;
	}

	switch (c->frequency) {
	case SBC_SAMPLING_FREQ_16000:
		transport->sampling = 16000;
		break;
	case SBC_SAMPLING_FREQ_32000:
		transport->sampling = 32000;
		break;
	case SBC_SAMPLING_FREQ_44100:
		transport->sampling = 44100;
		break;
	case SBC_SAMPLING_FREQ_48000:
		transport->sampling = 48000;
		break;
	}

}

static void ctl_thread_cmd_ping(const struct request *req, int fd, void *arg) {
	(void)req;
	(void)arg;
	static const struct msg_status status = { STATUS_CODE_PONG };
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_list_devices(const struct request *req, int fd, void *arg) {
	(void)req;

	struct ba_setup *setup = (struct ba_setup *)arg;
	static const struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_device device;
	GHashTableIter iter_d;
	struct ba_device *d;
	gpointer _tmp;

	pthread_mutex_lock(&setup->devices_mutex);

	for (g_hash_table_iter_init(&iter_d, setup->devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)&d); ) {

		bacpy(&device.addr, &d->addr);
		strncpy(device.name, d->name, sizeof(device.name) - 1);
		device.name[sizeof(device.name) - 1] = '\0';

		send(fd, &device, sizeof(device), MSG_NOSIGNAL);
	}

	pthread_mutex_unlock(&setup->devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_list_transports(const struct request *req, int fd, void *arg) {
	(void)req;

	struct ba_setup *setup = (struct ba_setup *)arg;
	static const struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_transport transport;
	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t;
	gpointer _tmp;

	pthread_mutex_lock(&setup->devices_mutex);

	for (g_hash_table_iter_init(&iter_d, setup->devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)&d); )
		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, &_tmp, (gpointer)&t); ) {
			_ctl_transport(d, t, &transport);
			send(fd, &transport, sizeof(transport), MSG_NOSIGNAL);
		}

	pthread_mutex_unlock(&setup->devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_get_transport(const struct request *req, int fd, void *arg) {

	struct ba_setup *setup = (struct ba_setup *)arg;
	struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_transport transport;
	struct ba_device *d;
	struct ba_transport *t;

	pthread_mutex_lock(&setup->devices_mutex);

	if (_transport_lookup(setup->devices, &req->addr, req->profile, &d, &t) != 0) {
		status.code = STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	}

	_ctl_transport(d, t, &transport);
	send(fd, &transport, sizeof(transport), MSG_NOSIGNAL);

fail:
	pthread_mutex_unlock(&setup->devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_set_transport_volume(const struct request *req, int fd, void *arg) {

	struct ba_setup *setup = (struct ba_setup *)arg;
	struct msg_status status = { STATUS_CODE_SUCCESS };
	struct ba_device *d;
	struct ba_transport *t;

	pthread_mutex_lock(&setup->devices_mutex);

	if (_transport_lookup(setup->devices, &req->addr, req->profile, &d, &t) != 0) {
		status.code = STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	}

	debug("Setting volume for %s profile %d: %d [%s]", batostr_(&req->addr),
			req->profile, req->volume, req->muted ? "off" : "on");

	t->muted = req->muted;
	t->volume = req->volume;

fail:
	pthread_mutex_unlock(&setup->devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static int ctl_pcm_release(struct ba_transport *t) {

	t->release_pcm = NULL;
	t->pcm_client = -1;

	/* The FIFO state should be managed by the controller module itself. Opened
	 * PCM connection has to be preserved among many transport acquisitions. */

	if (t->pcm_fifo != NULL) {
		debug("Cleaning PCM FIFO: %s", t->pcm_fifo);
		unlink(t->pcm_fifo);
		free(t->pcm_fifo);
		t->pcm_fifo = NULL;
	}

	if (t->pcm_fd != -1) {
		debug("Closing PCM: %d", t->pcm_fd);
		close(t->pcm_fd);
		t->pcm_fd = -1;
	}

	return 0;
}

static void ctl_thread_cmd_open_pcm(const struct request *req, int fd, void *arg) {

	struct ba_setup *setup = (struct ba_setup *)arg;
	struct msg_status status = { STATUS_CODE_SUCCESS };
	struct msg_pcm pcm;
	struct ba_device *d;
	struct ba_transport *t;
	char addr[18];

	ba2str(&req->addr, addr);
	snprintf(pcm.fifo, sizeof(pcm.fifo), BLUEALSA_RUN_STATE_DIR "/%s-%s-%u",
			setup->hci_dev.name, addr, req->profile);
	pcm.fifo[sizeof(pcm.fifo) - 1] = '\0';

	debug("PCM requested for %s profile %d", addr, req->profile);

	pthread_mutex_lock(&setup->devices_mutex);

	if (_transport_lookup(setup->devices, &req->addr, req->profile, &d, &t) != 0) {
		status.code = STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	}

	if (t->pcm_fifo != NULL) {
		debug("PCM already requested: %d", t->pcm_fd);
		status.code = STATUS_CODE_DEVICE_BUSY;
		goto fail;
	}

	_ctl_transport(d, t, &pcm.transport);

	if (mkfifo(pcm.fifo, 0660) != 0) {
		error("Couldn't create FIFO: %s", strerror(errno));
		status.code = STATUS_CODE_ERROR_UNKNOWN;
		goto fail;
	}

	/* During the mkfifo() call the FIFO mode is modified by the process umask,
	 * so the post-creation correction is required. */
	if (chmod(pcm.fifo, 0660) == -1)
		goto fail;
	if (chown(pcm.fifo, -1, setup->gid_audio) == -1)
		goto fail;

	/* for source profile we need to open transport by ourself */
	if (t->profile == TRANSPORT_PROFILE_A2DP_SOURCE)
		if (transport_acquire(t) == -1) {
			status.code = STATUS_CODE_ERROR_UNKNOWN;
			unlink(pcm.fifo);
			goto fail;
		}

	/* XXX: This will notify our forward transport IO thread, that the FIFO has
	 *      just been created, so it is possible to open it. Backward IO thread
	 *      should not be started before the PCM open request has been made, so
	 *      this "notification" mechanism does not apply. */
	t->pcm_fifo = strdup(pcm.fifo);

	t->release_pcm = ctl_pcm_release;
	t->pcm_client = fd;

	send(fd, &pcm, sizeof(pcm), MSG_NOSIGNAL);

fail:
	pthread_mutex_unlock(&setup->devices_mutex);
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void *ctl_thread(void *arg) {
	struct ba_setup *setup = (struct ba_setup *)arg;

	static void (*commands[__COMMAND_MAX])(const struct request *, int, void *) = {
		[COMMAND_PING] = ctl_thread_cmd_ping,
		[COMMAND_LIST_DEVICES] = ctl_thread_cmd_list_devices,
		[COMMAND_LIST_TRANSPORTS] = ctl_thread_cmd_list_transports,
		[COMMAND_GET_TRANSPORT] = ctl_thread_cmd_get_transport,
		[COMMAND_SET_TRANSPORT_VOLUME] = ctl_thread_cmd_set_transport_volume,
		[COMMAND_OPEN_PCM] = ctl_thread_cmd_open_pcm,
	};

	struct pollfd *pfd;
	struct request request;
	ssize_t len;
	int i;

	debug("Starting controller loop");
	while (setup->ctl_thread_created) {

		if (poll(setup->ctl_pfds, 1 + BLUEALSA_MAX_CLIENTS, -1) == -1) {
			error("Controller poll error: %s", strerror(errno));
			break;
		}

		/* Clients handling loop will update this variable to point to the
		 * first available client structure, which might be later used by
		 * the connection handling loop. */
		pfd = NULL;

		/* handle data transmission with connected clients */
		for (i = 1; i < 1 + BLUEALSA_MAX_CLIENTS; i++) {
			const int fd = setup->ctl_pfds[i].fd;

			if (fd == -1) {
				pfd = &setup->ctl_pfds[i];
				continue;
			}

			if (setup->ctl_pfds[i].revents & POLLIN) {

				if ((len = recv(fd, &request, sizeof(request), MSG_DONTWAIT)) != sizeof(request)) {
					/* if the request cannot be retrieved, release resources */

					if (len == 0)
						debug("Client closed connection: %d", fd);
					else
						debug("Invalid request length: %zd != %zd", len, sizeof(request));

					struct ba_transport *t;
					if ((t = transport_lookup_pcm_client(setup->devices, fd)) != NULL)
						t->release_pcm(t);

					setup->ctl_pfds[i].fd = -1;
					close(fd);
					continue;
				}

				/* validate and execute requested command */
				if (request.command >= __COMMAND_MAX)
					warn("Invalid command: %u", request.command);
				else if (commands[request.command] != NULL)
					commands[request.command](&request, fd, setup);

			}

		}

		/* process new connections to our controller */
		if (setup->ctl_pfds[0].revents & POLLIN && pfd != NULL) {
			pfd->fd = accept(setup->ctl_pfds[0].fd, NULL, NULL);
			debug("New client accepted: %d", pfd->fd);
		}

		debug("+-+-");
	}

	debug("Exiting controller thread");
	return NULL;
}

int ctl_thread_init(struct ba_setup *setup) {

	if (setup->ctl_thread_created) {
		/* thread is already created */
		errno = EISCONN;
		return -1;
	}

	{ /* initialize (mark as closed) all sockets */
		int i;
		for (i = 0; i < 1 + BLUEALSA_MAX_CLIENTS; i++) {
			setup->ctl_pfds[i].events = POLLIN;
			setup->ctl_pfds[i].fd = -1;
		}
	}

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", setup->hci_dev.name);

	if (mkdir(BLUEALSA_RUN_STATE_DIR, 0755) == -1 && errno != EEXIST)
		goto fail;
	if ((setup->ctl_pfds[0].fd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1)
		goto fail;
	if (bind(setup->ctl_pfds[0].fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1)
		goto fail;
	setup->ctl_socket_created = 1;
	if (chmod(saddr.sun_path, 0660) == -1)
		goto fail;
	if (chown(saddr.sun_path, -1, setup->gid_audio) == -1)
		goto fail;
	if (listen(setup->ctl_pfds[0].fd, 2) == -1)
		goto fail;

	setup->ctl_thread_created = 1;
	if ((errno = pthread_create(&setup->ctl_thread, NULL, ctl_thread, setup)) != 0) {
		setup->ctl_thread_created = 0;
		goto fail;
	}

	/* name controller thread - for aesthetic purposes only */
	pthread_setname_np(setup->ctl_thread, "bactl");

	return 0;

fail:
	ctl_free(setup);
	return -1;
}

void ctl_free(struct ba_setup *setup) {

	int created = setup->ctl_thread_created;
	int i, err;

	setup->ctl_thread_created = 0;

	for (i = 0; i < 1 + BLUEALSA_MAX_CLIENTS; i++)
		if (setup->ctl_pfds[i].fd != -1)
			close(setup->ctl_pfds[i].fd);

	if (created) {
		pthread_cancel(setup->ctl_thread);
		if ((err = pthread_join(setup->ctl_thread, NULL)) != 0)
			error("Couldn't join controller thread: %s", strerror(err));
	}

	if (setup->ctl_socket_created) {
		char tmp[256] = BLUEALSA_RUN_STATE_DIR "/";
		unlink(strcat(tmp, setup->hci_dev.name));
		setup->ctl_socket_created = 0;
	}

}
