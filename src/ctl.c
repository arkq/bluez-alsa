/*
 * BlueALSA - ctl.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
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
#include "bluez-iface.h"
#include "bluez.h"
#include "hfp.h"
#include "transport.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/log.h"

#define ctl_pfds_idx(i) g_array_index(config.ctl.pfds, struct pollfd, i)
#define ctl_subs_idx(i) g_array_index(config.ctl.subs, enum ba_event, i)

/**
 * Looks up a transport matching BT address and profile.
 *
 * This function is not thread-safe. It returns references to objects managed
 * by the devices hash-table. If the devices hash-table is modified in some
 * other thread, it may result in an undefined behavior.
 *
 * @param devices Address of the hash-table with connected devices.
 * @param addr Address to the structure with the looked up BT address.
 * @param type Looked up PCM type with stream mask.
 * @param t Address, where the transport structure pointer should be stored.
 * @return If the lookup succeeded, this function returns 0. Otherwise, -1 or
 *   -2 is returned respectively for not found device and not found stream.
 *   Upon error value of the transport pointer is undefined. */
static int _transport_lookup(GHashTable *devices, const bdaddr_t *addr,
		uint8_t type, struct ba_transport **t) {

	bool device_found = false;
	GHashTableIter iter_d, iter_t;
	struct ba_device *d;

	for (g_hash_table_iter_init(&iter_d, devices);
			g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d); ) {

		if (bacmp(&d->addr, addr) != 0)
			continue;

		device_found = true;

		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, NULL, (gpointer)t); )
			switch (BA_PCM_TYPE(type)) {
			case BA_PCM_TYPE_NULL:
				continue;
			case BA_PCM_TYPE_A2DP:
				if ((*t)->type != TRANSPORT_TYPE_A2DP)
					continue;
				if (type & BA_PCM_STREAM_PLAYBACK &&
						(*t)->profile == BLUETOOTH_PROFILE_A2DP_SOURCE)
					return 0;
				if (type & BA_PCM_STREAM_CAPTURE &&
						(*t)->profile == BLUETOOTH_PROFILE_A2DP_SINK)
					return 0;
				continue;
			case BA_PCM_TYPE_SCO:
				if ((*t)->type == TRANSPORT_TYPE_SCO)
					return 0;
				continue;
			}

	}

	return device_found ? -2 : -1;
}

static int _transport_lookup_rfcomm(GHashTable *devices, const bdaddr_t *addr,
		struct ba_transport **t) {

	GHashTableIter iter_d, iter_t;
	struct ba_device *d;

	for (g_hash_table_iter_init(&iter_d, devices);
			g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d); ) {

		if (bacmp(&d->addr, addr) != 0)
			continue;

		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, NULL, (gpointer)t); )
			if ((*t)->type == TRANSPORT_TYPE_RFCOMM)
				return 0;

	}

	return -1;
}

/**
 * Get transport PCM structure.
 *
 * @param t Pointer to the transport structure.
 * @param type PCM type with stream mask.
 * @return On success address of the PCM structure is returned. If the PCM
 *   structure can not be determined, NULL is returned. */
static struct ba_pcm *_transport_get_pcm(struct ba_transport *t, uint8_t type) {
	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		return &t->a2dp.pcm;
	case TRANSPORT_TYPE_RFCOMM:
		debug("Trying to get PCM for RFCOMM transport... that's nuts");
		break;
	case TRANSPORT_TYPE_SCO:
		if (type & BA_PCM_STREAM_PLAYBACK)
			return &t->sco.spk_pcm;
		if (type & BA_PCM_STREAM_CAPTURE)
			return &t->sco.mic_pcm;
	}
	return NULL;
}

/**
 * Release transport resources acquired by the controller module. */
static void _transport_release_pcm(struct ba_transport *t, int client) {
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

static void _ctl_transport(const struct ba_transport *t, struct ba_msg_transport *transport) {

	bacpy(&transport->addr, &t->device->addr);

	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		transport->type = BA_PCM_TYPE_A2DP | (t->profile == BLUETOOTH_PROFILE_A2DP_SOURCE ?
				BA_PCM_STREAM_PLAYBACK : BA_PCM_STREAM_CAPTURE);
		transport->ch1_muted = t->a2dp.ch1_muted;
		transport->ch1_volume = t->a2dp.ch1_volume;
		transport->ch2_muted = t->a2dp.ch2_muted;
		transport->ch2_volume = t->a2dp.ch2_volume;
		transport->delay = t->a2dp.delay;
		break;
	case TRANSPORT_TYPE_RFCOMM:
		transport->type = BA_PCM_TYPE_NULL;
		break;
	case TRANSPORT_TYPE_SCO:
		transport->type = BA_PCM_TYPE_SCO | BA_PCM_STREAM_PLAYBACK | BA_PCM_STREAM_CAPTURE;
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

static void ctl_thread_cmd_ping(const struct ba_request *req, int fd) {
	(void)req;
	static const struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_subscribe(const struct ba_request *req, int fd) {

	static const struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	size_t i;

	for (i = __CTL_IDX_MAX; i < config.ctl.pfds->len; i++)
		if (ctl_pfds_idx(i).fd == fd)
			ctl_subs_idx(i - __CTL_IDX_MAX) = req->events;

	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_list_devices(const struct ba_request *req, int fd) {
	(void)req;

	static const struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	struct ba_msg_device device;
	GHashTableIter iter_d;
	struct ba_device *d;

	bluealsa_devpool_mutex_lock();

	for (g_hash_table_iter_init(&iter_d, config.devices);
			g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d); ) {

		bacpy(&device.addr, &d->addr);
		strncpy(device.name, d->name, sizeof(device.name) - 1);
		device.name[sizeof(device.name) - 1] = '\0';

		device.battery = d->battery.enabled;
		device.battery_level = d->battery.level;

		send(fd, &device, sizeof(device), MSG_NOSIGNAL);
	}

	bluealsa_devpool_mutex_unlock();
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_list_transports(const struct ba_request *req, int fd) {
	(void)req;

	static const struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	struct ba_msg_transport transport;
	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t;

	bluealsa_devpool_mutex_lock();

	for (g_hash_table_iter_init(&iter_d, config.devices);
			g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d); )
		for (g_hash_table_iter_init(&iter_t, d->transports);
				g_hash_table_iter_next(&iter_t, NULL, (gpointer)&t); ) {
			_ctl_transport(t, &transport);
			send(fd, &transport, sizeof(transport), MSG_NOSIGNAL);
		}

	bluealsa_devpool_mutex_unlock();
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_transport_get(const struct ba_request *req, int fd) {

	struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	struct ba_msg_transport transport;
	struct ba_transport *t;

	bluealsa_devpool_mutex_lock();

	switch (_transport_lookup(config.devices, &req->addr, req->type, &t)) {
	case -1:
		status.code = BA_STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	case -2:
		status.code = BA_STATUS_CODE_STREAM_NOT_FOUND;
		goto fail;
	}

	_ctl_transport(t, &transport);
	send(fd, &transport, sizeof(transport), MSG_NOSIGNAL);

fail:
	bluealsa_devpool_mutex_unlock();
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_transport_set_volume(const struct ba_request *req, int fd) {

	struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	struct ba_transport *t;

	bluealsa_devpool_mutex_lock();

	switch (_transport_lookup(config.devices, &req->addr, req->type, &t)) {
	case -1:
		status.code = BA_STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	case -2:
		status.code = BA_STATUS_CODE_STREAM_NOT_FOUND;
		goto fail;
	}

	debug("Setting volume for %s type %#x: %d<>%d [%c%c]",
			batostr_(&req->addr), req->type, req->ch1_volume, req->ch2_volume,
			req->ch1_muted ? 'M' : 'O', req->ch2_muted ? 'M' : 'O');

	switch (BA_PCM_TYPE(req->type)) {
	case BA_PCM_TYPE_A2DP:

		t->a2dp.ch1_muted = req->ch1_muted;
		t->a2dp.ch2_muted = req->ch2_muted;
		t->a2dp.ch1_volume = req->ch1_volume;
		t->a2dp.ch2_volume = req->ch2_volume;

		if (config.a2dp.volume) {
			uint16_t volume = (req->ch1_muted | req->ch2_muted) ? 0 : MIN(req->ch1_volume, req->ch2_volume);
			g_dbus_set_property(config.dbus, t->dbus_owner, t->dbus_path,
					BLUEZ_IFACE_MEDIA_TRANSPORT, "Volume", g_variant_new_uint16(volume));
		}

		break;

	case BA_PCM_TYPE_SCO:

		t->sco.spk_muted = req->ch1_muted;
		t->sco.mic_muted = req->ch2_muted;
		t->sco.spk_gain = req->ch1_volume;
		t->sco.mic_gain = req->ch2_volume;

		if (t->sco.rfcomm != NULL)
			/* notify associated RFCOMM transport */
			transport_send_signal(t->sco.rfcomm, TRANSPORT_SET_VOLUME);

		break;
	}

	/* notify connected clients (including requester) */
	bluealsa_ctl_send_event(BA_EVENT_VOLUME_CHANGED, &req->addr, req->type);

fail:
	bluealsa_devpool_mutex_unlock();
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_pcm_open(const struct ba_request *req, int fd) {

	struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	struct ba_transport *t;
	struct ba_pcm *t_pcm;
	int pipefd[2];

	debug("PCM requested for %s type %#x", batostr_(&req->addr), req->type);

	bluealsa_devpool_mutex_lock();

	switch (_transport_lookup(config.devices, &req->addr, req->type, &t)) {
	case -1:
		status.code = BA_STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail_lookup;
	case -2:
		status.code = BA_STATUS_CODE_STREAM_NOT_FOUND;
		goto fail_lookup;
	}

	pthread_mutex_lock(&t->mutex);

	if (t->type == TRANSPORT_TYPE_SCO && t->codec == HFP_CODEC_UNDEFINED) {
		status.code = BA_STATUS_CODE_CODEC_NOT_SELECTED;
		goto final;
	}

	if ((t_pcm = _transport_get_pcm(t, req->type)) == NULL) {
		status.code = BA_STATUS_CODE_ERROR_UNKNOWN;
		goto final;
	}

	if (t_pcm->fd != -1) {
		debug("PCM already requested: %d", t_pcm->fd);
		status.code = BA_STATUS_CODE_DEVICE_BUSY;
		goto final;
	}

	if (pipe(pipefd) == -1) {
		error("Couldn't create FIFO: %s", strerror(errno));
		status.code = BA_STATUS_CODE_ERROR_UNKNOWN;
		goto final;
	}

	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr _align;
	} control_un;
	struct iovec io = { .iov_base = "", .iov_len = 1 };
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = control_un.buf,
		.msg_controllen = sizeof(control_un.buf),
	};

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	int *fdptr = (int *)CMSG_DATA(cmsg);

	if (req->type & BA_PCM_STREAM_PLAYBACK) {
		t_pcm->fd = pipefd[0];
		*fdptr = pipefd[1];
	}
	else {
		t_pcm->fd = pipefd[1];
		*fdptr = pipefd[0];
	}

	/* Notify our IO thread, that the FIFO has just been created - it may be
	 * used for poll() right away. */
	transport_send_signal(t, TRANSPORT_PCM_OPEN);

	/* A2DP source profile should be initialized (acquired) only if the audio
	 * is about to be transfered. It is most likely, that BT headset will not
	 * run voltage converter (power-on its circuit board) until the transport
	 * is acquired - in order to extend battery life. */
	if (t->profile == BLUETOOTH_PROFILE_A2DP_SOURCE)
		if (t->acquire(t) == -1) {
			status.code = BA_STATUS_CODE_ERROR_UNKNOWN;
			goto fail;
		}

	if (sendmsg(fd, &msg, 0) == -1)
		goto fail;

	t_pcm->client = fd;
	close(*fdptr);
	goto final;

fail:
	close(pipefd[0]);
	close(pipefd[1]);
	t_pcm->fd = -1;

final:
	pthread_mutex_unlock(&t->mutex);
fail_lookup:
	bluealsa_devpool_mutex_unlock();
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_pcm_close(const struct ba_request *req, int fd) {

	struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	struct ba_transport *t;
	struct ba_pcm *t_pcm;

	debug("PCM close for %s type %#x", batostr_(&req->addr), req->type);

	bluealsa_devpool_mutex_lock();

	switch (_transport_lookup(config.devices, &req->addr, req->type, &t)) {
	case -1:
		status.code = BA_STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail_lookup;
	case -2:
		status.code = BA_STATUS_CODE_STREAM_NOT_FOUND;
		goto fail_lookup;
	}

	pthread_mutex_lock(&t->mutex);

	if ((t_pcm = _transport_get_pcm(t, req->type)) == NULL) {
		status.code = BA_STATUS_CODE_ERROR_UNKNOWN;
		goto fail;
	}

	if (t_pcm->client != fd) {
		status.code = BA_STATUS_CODE_FORBIDDEN;
		goto fail;
	}

	_transport_release_pcm(t, fd);
	transport_send_signal(t, TRANSPORT_PCM_CLOSE);

fail:
	pthread_mutex_unlock(&t->mutex);
fail_lookup:
	bluealsa_devpool_mutex_unlock();
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_pcm_control(const struct ba_request *req, int fd) {

	struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	struct ba_transport *t;
	struct ba_pcm *t_pcm;

	bluealsa_devpool_mutex_lock();

	switch (_transport_lookup(config.devices, &req->addr, req->type, &t)) {
	case -1:
		status.code = BA_STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	case -2:
		status.code = BA_STATUS_CODE_STREAM_NOT_FOUND;
		goto fail;
	}
	if ((t_pcm = _transport_get_pcm(t, req->type)) == NULL) {
		status.code = BA_STATUS_CODE_ERROR_UNKNOWN;
		goto fail;
	}
	if (t_pcm->fd == -1 || t_pcm->client == -1) {
		status.code = BA_STATUS_CODE_ERROR_UNKNOWN;
		goto fail;
	}
	if (t_pcm->client != fd) {
		status.code = BA_STATUS_CODE_FORBIDDEN;
		goto fail;
	}

	switch (req->command) {
	case BA_COMMAND_PCM_PAUSE:
		transport_set_state(t, TRANSPORT_PAUSED);
		transport_send_signal(t, TRANSPORT_PCM_PAUSE);
		break;
	case BA_COMMAND_PCM_RESUME:
		transport_set_state(t, TRANSPORT_ACTIVE);
		transport_send_signal(t, TRANSPORT_PCM_RESUME);
		break;
	case BA_COMMAND_PCM_DRAIN:
		transport_drain_pcm(t);
		break;
	default:
		warn("Invalid PCM control command: %d", req->command);
	}

fail:
	bluealsa_devpool_mutex_unlock();
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void ctl_thread_cmd_rfcomm_send(const struct ba_request *req, int fd) {

	struct ba_msg_status status = { BA_STATUS_CODE_SUCCESS };
	struct ba_transport *t;

	bluealsa_devpool_mutex_lock();

	if (_transport_lookup_rfcomm(config.devices, &req->addr, &t) != 0) {
		status.code = BA_STATUS_CODE_DEVICE_NOT_FOUND;
		goto fail;
	}

	transport_send_rfcomm(t, req->rfcomm_command);

fail:
	bluealsa_devpool_mutex_unlock();
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
}

static void *ctl_thread(void *arg) {
	(void)arg;

	static void (*commands[__BA_COMMAND_MAX])(const struct ba_request *, int) = {
		[BA_COMMAND_PING] = ctl_thread_cmd_ping,
		[BA_COMMAND_SUBSCRIBE] = ctl_thread_cmd_subscribe,
		[BA_COMMAND_LIST_DEVICES] = ctl_thread_cmd_list_devices,
		[BA_COMMAND_LIST_TRANSPORTS] = ctl_thread_cmd_list_transports,
		[BA_COMMAND_TRANSPORT_GET] = ctl_thread_cmd_transport_get,
		[BA_COMMAND_TRANSPORT_SET_VOLUME] = ctl_thread_cmd_transport_set_volume,
		[BA_COMMAND_PCM_OPEN] = ctl_thread_cmd_pcm_open,
		[BA_COMMAND_PCM_CLOSE] = ctl_thread_cmd_pcm_close,
		[BA_COMMAND_PCM_PAUSE] = ctl_thread_cmd_pcm_control,
		[BA_COMMAND_PCM_RESUME] = ctl_thread_cmd_pcm_control,
		[BA_COMMAND_PCM_DRAIN] = ctl_thread_cmd_pcm_control,
		[BA_COMMAND_RFCOMM_SEND] = ctl_thread_cmd_rfcomm_send,
	};

	debug("Starting controller loop");
	while (config.ctl.thread_created) {

		if (poll((struct pollfd *)config.ctl.pfds->data, config.ctl.pfds->len, -1) == -1) {
			if (errno == EINTR)
				continue;
			error("Controller poll error: %s", strerror(errno));
			break;
		}

		size_t i;

		/* handle data transmission with connected clients */
		for (i = __CTL_IDX_MAX; i < config.ctl.pfds->len; i++)
			if (ctl_pfds_idx(i).revents & POLLIN) {

				const int fd = ctl_pfds_idx(i).fd;
				struct ba_request request;
				ssize_t len;

				if ((len = recv(fd, &request, sizeof(request), MSG_DONTWAIT)) != sizeof(request)) {
					/* if the request cannot be retrieved, release resources */

					if (len == 0)
						debug("Client closed connection: %d", fd);
					else
						debug("Invalid request length: %zd != %zd", len, sizeof(request));

					struct ba_transport *t;
					if ((t = transport_lookup_pcm_client(config.devices, fd)) != NULL) {
						_transport_release_pcm(t, fd);
						transport_send_signal(t, TRANSPORT_PCM_CLOSE);
					}

					g_array_remove_index_fast(config.ctl.pfds, i);
					g_array_remove_index_fast(config.ctl.subs, i - __CTL_IDX_MAX);
					close(fd);
					continue;
				}

				/* validate and execute requested command */
				if (request.command < __BA_COMMAND_MAX && commands[request.command] != NULL)
					commands[request.command](&request, fd);
				else
					warn("Invalid command: %u", request.command);

			}

		/* process new connections to our controller */
		if (ctl_pfds_idx(CTL_IDX_SRV).revents & POLLIN) {

			struct pollfd fd = { -1, POLLIN, 0 };
			uint16_t ver = 0;

			fd.fd = accept(ctl_pfds_idx(CTL_IDX_SRV).fd, NULL, NULL);
			debug("Received new connection: %d", fd.fd);

			errno = ETIMEDOUT;
			if (poll(&fd, 1, 500) <= 0 ||
					recv(fd.fd, &ver, sizeof(ver), MSG_DONTWAIT) != sizeof(ver)) {
				warn("Couldn't receive protocol version: %s", strerror(errno));
				close(fd.fd);
			}
			else if (ver != BLUEALSA_CRL_PROTO_VERSION) {
				warn("Invalid protocol version: %#06x != %#06x", ver, BLUEALSA_CRL_PROTO_VERSION);
				close(fd.fd);
			}
			else {
				debug("New client accepted: %d", fd.fd);
				g_array_append_val(config.ctl.pfds, fd);
				g_array_set_size(config.ctl.subs, config.ctl.subs->len + 1);
			}

		}

		/* generate notifications for subscribed clients */
		if (ctl_pfds_idx(CTL_IDX_EVT).revents & POLLIN) {

			struct ba_msg_event ev;
			size_t i;

			if (read(ctl_pfds_idx(CTL_IDX_EVT).fd, &ev, sizeof(ev)) == -1)
				warn("Couldn't read controller event: %s", strerror(errno));

			for (i = 0; i < config.ctl.subs->len; i++)
				if (ctl_subs_idx(i) & ev.events) {
					const int client = ctl_pfds_idx(i + __CTL_IDX_MAX).fd;
					debug("Sending notification: %B => %d", ev.events, client);
					send(client, &ev, sizeof(ev), MSG_NOSIGNAL);
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

	struct pollfd pfd = { .events = POLLIN };

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", config.hci_dev.name);

	if (mkdir(BLUEALSA_RUN_STATE_DIR, 0755) == -1 && errno != EEXIST) {
		error("Couldn't create run-state directory: %s", strerror(errno));
		goto fail;
	}

	if ((pfd.fd = socket(PF_UNIX, SOCK_SEQPACKET, 0)) == -1) {
		error("Couldn't create controller socket: %s", strerror(errno));
		goto fail;
	}
	g_array_append_val(config.ctl.pfds, pfd);
	if (bind(pfd.fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1) {
		error("Couldn't bind controller socket: %s", strerror(errno));
		goto fail;
	}
	config.ctl.socket_created = true;
	if (chmod(saddr.sun_path, 0660) == -1 ||
			chown(saddr.sun_path, -1, config.gid_audio) == -1) {
		error("Couldn't set permission for controller socket: %s", strerror(errno));
		goto fail;
	}
	if (listen(pfd.fd, 2) == -1) {
		error("Couldn't listen on controller socket: %s", strerror(errno));
		goto fail;
	}

	if (pipe(config.ctl.evt) == -1) {
		error("Couldn't create controller event PIPE: %s", strerror(errno));
		goto fail;
	}

	pfd.fd = config.ctl.evt[0];
	g_array_append_val(config.ctl.pfds, pfd);

	config.ctl.thread_created = true;
	if ((errno = pthread_create(&config.ctl.thread, NULL, ctl_thread, NULL)) != 0) {
		error("Couldn't create controller thread: %s", strerror(errno));
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

	for (i = 0; i < config.ctl.pfds->len; i++)
		close(ctl_pfds_idx(i).fd);
	if (config.ctl.evt[1] != -1)
		close(config.ctl.evt[1]);

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

/**
 * Send notification event to subscribed clients. */
int bluealsa_ctl_send_event(enum ba_event event, const bdaddr_t *addr, uint8_t type) {
	struct ba_msg_event ev = { .events = event, .addr = *addr, .type = type };
	return write(config.ctl.evt[1], &ev, sizeof(ev));
}
