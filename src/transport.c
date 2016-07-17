/*
 * bluealsa - transport.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "transport.h"

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "io.h"
#include "log.h"


static void io_thread_create(struct ba_transport *t) {

	static void *(*routine[__TRANSPORT_MAX])(void *) = {
		[TRANSPORT_A2DP_SOURCE] = io_thread_a2dp_sbc_forward,
		[TRANSPORT_A2DP_SINK] = io_thread_s2dp_sbc_backward,
	};

	const int type = t->type;
	int ret;

	if (routine[type] == NULL) {
		warn("Transport not implemented: %d", type);
		return;
	}

	if ((ret = pthread_create(&t->thread, NULL, routine[type], t)) != 0)
		error("Cannot create IO thread: %s", strerror(ret));
}

int transport_threads_init(void) {
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	return sigaction(SIGUSR1, &sigact, NULL);
}

struct ba_transport *transport_new(enum ba_transport_type type, const char *name) {

	struct ba_transport *t;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		return NULL;

	t->type = type;
	t->name = strdup(name);
	t->bt_fd = -1;
	t->pcm_fd = -1;

	return t;
}

void transport_free(struct ba_transport *t) {

	if (t == NULL)
		return;

	if (t->bt_fd != -1)
		close(t->bt_fd);
	if (t->pcm_fd != -1)
		close(t->pcm_fd);

	if (t->pcm_fifo != NULL) {
		/* XXX: During normal operation the FIFO node should be unlinked by the
		 *      alsa-pcm plugin in order to prevent data hijacking. However, a
		 *      proper cleaning on the server site will not hurt. */
		unlink(t->pcm_fifo);
		free(t->pcm_fifo);
	}

	free(t->name);
	free(t->dbus_owner);
	free(t->dbus_path);
	free(t->config);
	free(t);
}

int transport_set_dbus(struct ba_transport *t, DBusConnection *conn,
		const char *owner, const char *path) {
	t->dbus_conn = conn;
	free(t->dbus_owner);
	t->dbus_owner = strdup(owner);
	free(t->dbus_path);
	t->dbus_path = strdup(path);
	return 0;
}

int transport_set_codec(struct ba_transport *t, uint8_t codec,
		const uint8_t *config, size_t size) {

	t->codec = codec;
	t->config_size = size;

	if (size > 0) {
		free(t->config);
		t->config = malloc(sizeof(*t->config) * size);
		memcpy(t->config, config, size);
	}

	return 0;
}

int transport_set_state(struct ba_transport *t, enum ba_transport_state state) {
	debug("State transition: %d -> %d", t->state, state);

	if (t->state == state)
		return -1;

	t->state = state;

	switch (state) {
	case TRANSPORT_IDLE:
		/* TODO: Maybe use pthread_cancel() ? */
		pthread_kill(t->thread, SIGUSR1);
		pthread_join(t->thread, NULL);
		transport_release(t);
		break;
	case TRANSPORT_PENDING:
		transport_acquire(t);
		break;
	case TRANSPORT_ACTIVE:
		io_thread_create(t);
		break;
	}

	return 0;
}

int transport_set_state_from_string(struct ba_transport *t, const char *state) {

	if (strcmp(state, "idle") == 0)
		transport_set_state(t, TRANSPORT_IDLE);
	else if (strcmp(state, "pending") == 0)
		transport_set_state(t, TRANSPORT_PENDING);
	else if (strcmp(state, "active") == 0)
		transport_set_state(t, TRANSPORT_ACTIVE);
	else {
		warn("Invalid state: %s", state);
		return -1;
	}

	return 0;
}

int transport_acquire(struct ba_transport *t) {

	assert(t->dbus_conn != NULL && "D-Bus connection is not set");
	assert(t->dbus_owner != NULL && "D-Bus owner is not set");

	DBusMessage *msg, *rep;
	DBusError err;

	dbus_error_init(&err);

	msg = dbus_message_new_method_call(t->dbus_owner, t->dbus_path, "org.bluez.MediaTransport1", "Acquire");

	if (t->bt_fd != -1) {
		close(t->bt_fd);
		t->bt_fd = -1;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(t->dbus_conn, msg, -1, &err)) == NULL) {
		error("Cannot acquire transport: %s", err.message);
		dbus_error_free(&err);
	}
	else if (!dbus_message_get_args(rep, &err,
				DBUS_TYPE_UNIX_FD, &t->bt_fd,
				DBUS_TYPE_UINT16, &t->mtu_read,
				DBUS_TYPE_UINT16, &t->mtu_write,
				DBUS_TYPE_INVALID)) {
		error("Invalid D-Bus reply: %s", err.message);
		dbus_error_free(&err);
	}
	else
		debug("Acquired new transport: %d", t->bt_fd);

	dbus_message_unref(msg);
	dbus_message_unref(rep);
	return t->bt_fd;
}

int transport_release(struct ba_transport *t) {

	assert(t->dbus_conn != NULL && "D-Bus connection is not set");
	assert(t->dbus_owner != NULL && "D-Bus owner is not set");

	DBusMessage *msg;
	DBusError err;
	int ret = 0;

	dbus_error_init(&err);

	msg = dbus_message_new_method_call(t->dbus_owner, t->dbus_path, "org.bluez.MediaTransport1", "Release");
	dbus_connection_send_with_reply_and_block(t->dbus_conn, msg, -1, &err);

	if (dbus_error_is_set(&err)) {
		error("Cannot release transport: %s", err.message);
		dbus_error_free(&err);
		ret = -1;
	}
	else {
		close(t->bt_fd);
		t->bt_fd = -1;
	}

	dbus_message_unref(msg);
	return ret;
}

const char *transport_type_to_string(enum ba_transport_type type) {
	switch (type) {
	case TRANSPORT_A2DP_SOURCE:
		return "A2DP Source";
	case TRANSPORT_A2DP_SINK:
		return "A2DP Sink";
	case TRANSPORT_HFP:
		return "HFP";
	case TRANSPORT_HSP:
		return "HSP";
	default:
		return "N/A";
	};
}
