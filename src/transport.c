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

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "a2dp-codecs.h"
#include "io.h"
#include "log.h"


static void io_thread_create(struct ba_transport *t) {

	int ret;
	void *(*routine)(void *) = NULL;

	switch (t->profile) {
	case TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (t->codec) {
		case A2DP_CODEC_SBC:
			routine = io_thread_a2dp_sbc_backward;
			break;
		case A2DP_CODEC_MPEG12:
		case A2DP_CODEC_MPEG24:
		default:
			warn("Codec not supported: %u", t->codec);
		}
		break;
	case TRANSPORT_PROFILE_A2DP_SINK:
		switch (t->codec) {
		case A2DP_CODEC_SBC:
			routine = io_thread_a2dp_sbc_forward;
			break;
		case A2DP_CODEC_MPEG12:
		case A2DP_CODEC_MPEG24:
		default:
			warn("Codec not supported: %u", t->codec);
		}
		break;
	case TRANSPORT_PROFILE_HFP:
	case TRANSPORT_PROFILE_HSP:
	default:
		warn("Profile not implemented: %u", t->profile);
	}

	if (routine == NULL)
		return;

	if ((ret = pthread_create(&t->thread, NULL, routine, t)) != 0)
		error("Cannot create IO thread: %s", strerror(ret));
}

int transport_threads_init(void) {
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	return sigaction(SIGUSR1, &sigact, NULL);
}

struct ba_transport *transport_new(DBusConnection *conn, const char *dbus_owner,
		const char *dbus_path, const char *name, uint8_t profile, uint8_t codec,
		const uint8_t *config, size_t config_size) {

	struct ba_transport *t;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		return NULL;

	t->dbus_conn = conn;
	t->dbus_owner = strdup(dbus_owner);
	t->dbus_path = strdup(dbus_path);

	t->name = strdup(name);

	t->profile = profile;
	t->codec = codec;

	if (config_size > 0) {
		t->config = malloc(sizeof(*t->config) * config_size);
		t->config_size = config_size;
		memcpy(t->config, config, config_size);
	}

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
