/*
 * bluealsa - ctl.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

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
#include "bluez.h"
#include "log.h"
#include "transport.h"


static struct controller_ctl {

	int created;
	pthread_t thread;

	char *socket_path;
	struct pollfd pfds[1 + BLUEALSA_CTL_MAX_CLIENTS];

	char *hci_device;
	GHashTable *devices;

} ctl = {
	/* XXX: Other fields will be initialized properly
	 *      in the ctl_thread_init() function. */
	.created = 0,
};


/**
 * Convert a ctl transport type to the internal one.
 *
 * @param type The ctl transport type value.
 * @return Bluealsa transport type. If the given ctl transport type value is
 *   invalid (does not fit into the valid value range), or is not supported,
 *   this function returns TRANSPORT_DISABLED. */
static enum ba_transport_type _transport_type_ctl2ba(uint8_t type) {
	static const enum ba_transport_type tt[__CTL_TRANSPORT_TYPE_MAX] = {
		[CTL_TRANSPORT_TYPE_DISABLED] = TRANSPORT_DISABLED,
		[CTL_TRANSPORT_TYPE_A2DP_SOURCE] = TRANSPORT_A2DP_SOURCE,
		[CTL_TRANSPORT_TYPE_A2DP_SINK] = TRANSPORT_A2DP_SINK,
	};
	if (type >= __CTL_TRANSPORT_TYPE_MAX)
		return tt[CTL_TRANSPORT_TYPE_DISABLED];
	return tt[type];
}

/**
 * Convert internal transport type to the ctl one.
 *
 * @param type Bluealsa transport type.
 * @return Ctl transport type. */
static enum ctl_transport_type _transport_type_ba2ctl(enum ba_transport_type type) {
	static const enum ctl_transport_type tt[__TRANSPORT_MAX] = {
		[TRANSPORT_DISABLED] = CTL_TRANSPORT_TYPE_DISABLED,
		[TRANSPORT_A2DP_SOURCE] = CTL_TRANSPORT_TYPE_A2DP_SOURCE,
		[TRANSPORT_A2DP_SINK] = CTL_TRANSPORT_TYPE_A2DP_SINK,
		[TRANSPORT_HFP] = CTL_TRANSPORT_TYPE_HFP,
		[TRANSPORT_HSP] = CTL_TRANSPORT_TYPE_HSP,
	};
	return tt[type];
}

static int _transport_lookup(const bdaddr_t *addr, uint8_t type,
		struct ba_device **d, struct ba_transport **t) {

	GHashTableIter iter_d, iter_t;
	gpointer _tmp;

	/* TODO: acquire mutex for transport modifications */

	for (g_hash_table_iter_init(&iter_d, ctl.devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)d); ) {

		if (bacmp(&(*d)->addr, addr) != 0)
			continue;

		for (g_hash_table_iter_init(&iter_t, (*d)->transports);
				g_hash_table_iter_next(&iter_t, &_tmp, (gpointer)t); )
			if (_transport_type_ctl2ba(type) == (*t)->type)
				return 0;

	}

	return -1;
}

static int _ctl_transport(const struct ba_device *d, const struct ba_transport *t,
		struct ctl_transport *transport) {

	bacpy(&transport->addr, &d->addr);
	transport->type = _transport_type_ba2ctl(t->type);

	strncpy(transport->name, t->name, sizeof(transport->name) - 1);
	transport->name[sizeof(transport->name)] = '\0';

	transport->volume = t->volume;

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
		transport->frequency = CTL_TRANSPORT_SP_FREQ_16000;
		break;
	case SBC_SAMPLING_FREQ_32000:
		transport->frequency = CTL_TRANSPORT_SP_FREQ_32000;
		break;
	case SBC_SAMPLING_FREQ_44100:
		transport->frequency = CTL_TRANSPORT_SP_FREQ_44100;
		break;
	case SBC_SAMPLING_FREQ_48000:
		transport->frequency = CTL_TRANSPORT_SP_FREQ_48000;
		break;
	}

	return 0;
}

static void ctl_thread_cmd_ping(const struct ctl_request *req, int fd) {
	(void)req;
	time_t ts = time(NULL);
	send(fd, &ts, sizeof(ts), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_list_devices(const struct ctl_request *req, int fd) {
	(void)req;

	struct ctl_device device;
	GHashTableIter iter_d;
	struct ba_device *d;
	gpointer _tmp;

	/* TODO: acquire mutex for transport modifications */

	for (g_hash_table_iter_init(&iter_d, ctl.devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)&d); ) {

		bacpy(&device.addr, &d->addr);
		strncpy(device.name, d->name, sizeof(device.name) - 1);
		device.name[sizeof(device.name)] = '\0';

		send(fd, &device, sizeof(device), MSG_NOSIGNAL);
	}

	send(fd, CTL_END, 1, MSG_NOSIGNAL);
}

static void ctl_thread_cmd_list_transports(const struct ctl_request *req, int fd) {
	(void)req;

	struct ctl_transport transport;
	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t;
	gpointer _tmp;

	/* TODO: acquire mutex for transport modifications */

	for (g_hash_table_iter_init(&iter_d, ctl.devices);
			g_hash_table_iter_next(&iter_d, &_tmp, (gpointer)&d); )
		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, &_tmp, (gpointer)&t); ) {

			if (_ctl_transport(d, t, &transport) != 0)
				continue;

			send(fd, &transport, sizeof(transport), MSG_NOSIGNAL);
		}

	send(fd, CTL_END, 1, MSG_NOSIGNAL);
}

static void ctl_thread_cmd_get_transport(const struct ctl_request *req, int fd) {

	struct ctl_transport transport;
	struct ba_device *d;
	struct ba_transport *t;

	if (_transport_lookup(&req->addr, req->type, &d, &t) != 0)
		goto fail;
	if (_ctl_transport(d, t, &transport) != 0)
		goto fail;

	send(fd, &transport, sizeof(transport), MSG_NOSIGNAL);
	return;

fail:
	send(fd, CTL_END, 1, MSG_NOSIGNAL);
}

static void ctl_thread_cmd_open_pcm(const struct ctl_request *req, int fd) {

	struct ctl_pcm pcm;
	struct ba_device *d;
	struct ba_transport *t;

	if (_transport_lookup(&req->addr, req->type, &d, &t) != 0)
		goto fail;
	if (t->pcm_fifo != NULL) {
		debug("PCM already requested: %u", t->pcm_fd);
		goto fail;
	}
	if (_ctl_transport(d, t, &pcm.transport) != 0)
		goto fail;

	snprintf(pcm.fifo, sizeof(pcm.fifo), BLUEALSA_RUN_STATE_DIR "/%s-%s-%u",
			ctl.hci_device, batostr(&d->addr), req->type);
	pcm.fifo[sizeof(pcm.fifo)] = '\0';

	if (mkfifo(pcm.fifo, 0660) != 0) {
		error("Cannot create FIFO: %s", strerror(errno));
		goto fail;
	}

	/* XXX: This will notify the transport IO thread, that there was a request
	 *      for opening a FIFO and that the node has been just created. */
	t->pcm_fifo = strdup(pcm.fifo);

	send(fd, &pcm, sizeof(pcm), MSG_NOSIGNAL);
	return;

fail:
	send(fd, CTL_END, 1, MSG_NOSIGNAL);
}

static void *ctl_thread(void *arg) {
	(void)arg;

	static void (*commands[__CTL_COMMAND_MAX])(const struct ctl_request *, int) = {
		[CTL_COMMAND_PING] = ctl_thread_cmd_ping,
		[CTL_COMMAND_LIST_DEVICES] = ctl_thread_cmd_list_devices,
		[CTL_COMMAND_LIST_TRANSPORTS] = ctl_thread_cmd_list_transports,
		[CTL_COMMAND_GET_TRANSPORT] = ctl_thread_cmd_get_transport,
		[CTL_COMMAND_OPEN_PCM] = ctl_thread_cmd_open_pcm,
	};

	struct pollfd *pfd;
	struct ctl_request request;
	ssize_t len;
	int i;

	debug("Starting controller loop");
	while (ctl.created) {

		if (poll(ctl.pfds, 1 + BLUEALSA_CTL_MAX_CLIENTS, -1) == -1)
			break;

		/* Clients handling loop will update this variable to point to the
		 * first available client structure, which might be later used by
		 * the connection handling loop. */
		pfd = NULL;

		/* handle data transmission with connected clients */
		for (i = 1; i < 1 + BLUEALSA_CTL_MAX_CLIENTS; i++) {
			const int fd = ctl.pfds[i].fd;

			if (fd == -1) {
				pfd = &ctl.pfds[i];
				continue;
			}

			if (ctl.pfds[i].revents & POLLIN) {

				if ((len = recv(fd, &request, sizeof(request), MSG_DONTWAIT)) <= 0) {
					debug("Client closed connection: %d", fd);
					ctl.pfds[i].fd = -1;
					close(fd);
					continue;
				}

				if (len != sizeof(request)) {
					debug("Invalid request length: %zd != %zd", len, sizeof(request));
					ctl.pfds[i].fd = -1;
					close(fd);
					continue;
				}

				/* validate and execute requested command */
				if (request.command >= __CTL_COMMAND_MAX)
					warn("Invalid command: %u", request.command);
				else if (commands[request.command] != NULL)
					commands[request.command](&request, fd);

			}

		}

		/* process new connections to our controller */
		if (ctl.pfds[0].revents & POLLIN && pfd != NULL) {
			pfd->fd = accept(ctl.pfds[0].fd, NULL, NULL);
			debug("New client accepted: %d", pfd->fd);
		}

		debug("+-+-");
	}

	debug("Exiting from controller thread");
	return NULL;
}

int ctl_thread_init(const char *device, void *userdata) {

	if (ctl.created) {
		/* thread is already created */
		errno = EISCONN;
		return -1;
	}

	{ /* initialize (mark as closed) all sockets */
		int i;
		for (i = 0; i < 1 + BLUEALSA_CTL_MAX_CLIENTS; i++) {
			ctl.pfds[i].events = POLLIN;
			ctl.pfds[i].fd = -1;
		}
	}

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", device);

	ctl.socket_path = strdup(saddr.sun_path);
	ctl.hci_device = strdup(device);
	ctl.devices = (GHashTable *)userdata;

	if ((ctl.pfds[0].fd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1)
		goto fail;
	if (bind(ctl.pfds[0].fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1)
		goto fail;
	if (listen(ctl.pfds[0].fd, 2) == -1)
		goto fail;

	ctl.created = 1;
	if ((errno = pthread_create(&ctl.thread, NULL, ctl_thread, NULL)) != 0) {
		ctl.created = 0;
		goto fail;
	}

	return 0;

fail:
	ctl_free();
	return -1;
}

void ctl_free() {

	int i;

	if (ctl.created) {
		int err;
		ctl.created = 0;
		if ((err = pthread_join(ctl.thread, NULL)) != 0)
			error("Cannot join controller thread: %s", strerror(err));
	}

	for (i = 0; i < 1 + BLUEALSA_CTL_MAX_CLIENTS; i++)
		if (ctl.pfds[i].fd != -1)
			close(ctl.pfds[i].fd);

	if (ctl.socket_path != NULL)
		unlink(ctl.socket_path);

	free(ctl.socket_path);
	free(ctl.hci_device);
}
