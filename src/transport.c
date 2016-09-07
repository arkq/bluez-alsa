/*
 * BlueALSA - transport.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include "transport.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gunixfdlist.h>

#include "a2dp-codecs.h"
#include "io.h"
#include "log.h"


static int io_thread_create(struct ba_transport *t) {

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
		case A2DP_CODEC_ATRAC:
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
		case A2DP_CODEC_ATRAC:
		default:
			warn("Codec not supported: %u", t->codec);
		}
		break;
	case TRANSPORT_PROFILE_HFP:
	case TRANSPORT_PROFILE_HSP:
	default:
		warn("Profile not supported: %u", t->profile);
	}

	if (routine == NULL)
		return -1;

	/* initialize time accounting */
	t->ts_paused.tv_sec = 0;
	t->ts_paused.tv_nsec = 0;

	if ((ret = pthread_create(&t->thread, NULL, routine, t)) != 0) {
		error("Couldn't create IO thread: %s", strerror(ret));
		return -1;
	}

	pthread_setname_np(t->thread, "baio");
	debug("Created new IO thread: %s", t->name);
	return 0;
}

struct ba_device *device_new(bdaddr_t *addr, const char *name) {

	struct ba_device *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return NULL;

	bacpy(&d->addr, addr);
	d->name = strdup(name);
	d->transports = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)transport_free);

	return d;
}

void device_free(struct ba_device *d) {

	if (d == NULL)
		return;

	g_hash_table_unref(d->transports);
	free(d->name);
	free(d);
}

struct ba_transport *transport_new(GDBusConnection *conn, const char *dbus_owner,
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
	t->volume = 100;

	if (config_size > 0) {
		t->config = malloc(config_size);
		t->config_size = config_size;
		memcpy(t->config, config, config_size);
	}

	t->state = TRANSPORT_IDLE;
	t->bt_fd = -1;
	t->pcm_client = -1;
	t->pcm_fd = -1;

	return t;
}

void transport_free(struct ba_transport *t) {

	if (t == NULL)
		return;

	debug("Freeing transport: %s", t->name);

	/* If the transport is active, prior to releasing resources, we have to
	 * terminate the IO thread (or at least make sure it is not running any
	 * more). Not doing so might result in an undefined behavior or even a
	 * race condition (closed and reused file descriptor). */
	if (t->state == TRANSPORT_ACTIVE) {
		pthread_cancel(t->thread);
		pthread_join(t->thread, NULL);
	}

	/* if possible, try to release resources gracefully */
	if (t->release != NULL)
		t->release(t);

	if (t->bt_fd != -1)
		close(t->bt_fd);

	transport_release_pcm(t);

	free(t->name);
	free(t->dbus_owner);
	free(t->dbus_path);
	free(t->config);
	free(t);
}

struct ba_transport *transport_lookup(GHashTable *devices, const char *key) {

	GHashTableIter iter;
	struct ba_device *d;
	struct ba_transport *t;
	gpointer _key;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, &_key, (gpointer)&d)) {
		if ((t = g_hash_table_lookup(d->transports, key)) != NULL)
			return t;
	}

	return NULL;
}

struct ba_transport *transport_lookup_pcm_client(GHashTable *devices, int client) {

	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t;
	gpointer tmp;

	g_hash_table_iter_init(&iter_d, devices);
	while (g_hash_table_iter_next(&iter_d, &tmp, (gpointer)&d)) {
		g_hash_table_iter_init(&iter_t, d->transports);
		while (g_hash_table_iter_next(&iter_t, &tmp, (gpointer)&t))
			if (t->pcm_client == client)
				return t;
	}

	return NULL;
}

gboolean transport_remove(GHashTable *devices, const char *key) {

	GHashTableIter iter;
	struct ba_device *d;
	gpointer _key;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, &_key, (gpointer)&d)) {
		if (g_hash_table_remove(d->transports, key)) {
			if (g_hash_table_size(d->transports) == 0)
				g_hash_table_iter_remove(&iter);
			return TRUE;
		}
	}

	return FALSE;
}

int transport_set_state(struct ba_transport *t, enum ba_transport_state state) {
	debug("State transition: %d -> %d", t->state, state);

	if (t->state == state)
		return 0;

	int ret = -1;

	t->state = state;

	switch (state) {
	case TRANSPORT_IDLE:
		pthread_cancel(t->thread);
		ret = pthread_join(t->thread, NULL);
		break;
	case TRANSPORT_PENDING:
		ret = transport_acquire(t);
		break;
	case TRANSPORT_ACTIVE:
		ret = io_thread_create(t);
		break;
	}

	/* something went wrong, so go back to idle */
	if (ret == -1)
		return transport_set_state(t, TRANSPORT_IDLE);

	return ret;
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

	GDBusMessage *msg, *rep;
	GUnixFDList *fd_list;
	GError *err = NULL;

	msg = g_dbus_message_new_method_call(t->dbus_owner, t->dbus_path, "org.bluez.MediaTransport1",
			t->state == TRANSPORT_PENDING ? "TryAcquire" : "Acquire");

	if (t->bt_fd != -1) {
		warn("Closing dangling BT socket: %d", t->bt_fd);
		close(t->bt_fd);
		t->bt_fd = -1;
	}

	if ((rep = g_dbus_connection_send_message_with_reply_sync(t->dbus_conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	g_variant_get(g_dbus_message_get_body(rep), "(hqq)", (int32_t *)&t->bt_fd,
			(uint16_t *)&t->mtu_read, (uint16_t *)&t->mtu_write);

	fd_list = g_dbus_message_get_unix_fd_list(rep);
	t->bt_fd = g_unix_fd_list_get(fd_list, 0, &err);
	t->release = transport_release_bt;
	t->state = TRANSPORT_PENDING;

	debug("New transport: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

fail:
	g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't acquire transport: %s", err->message);
		g_error_free(err);
	}
	return t->bt_fd;
}

int transport_release_bt(struct ba_transport *t) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	/* If the transport has not been acquired, or it has been released already,
	 * there is no need to release it again. In fact, trying to release already
	 * closed transport will result in returning error message. */
	if (t->bt_fd == -1)
		return 0;

	debug("Releasing transport: %s", t->name);

	/* If the state is idle, it means that either transport was not acquired, or
	 * was released by the Bluez. In both cases there is no point in a explicit
	 * release request. It might even return error (e.g. not authorized). */
	if (t->state != TRANSPORT_IDLE) {

		msg = g_dbus_message_new_method_call(t->dbus_owner, t->dbus_path,
				"org.bluez.MediaTransport1", "Release");

		if ((rep = g_dbus_connection_send_message_with_reply_sync(t->dbus_conn, msg,
						G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
			goto fail;

		if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
			g_dbus_message_to_gerror(rep, &err);
			if (err->code == G_DBUS_ERROR_NO_REPLY) {
				/* If Bluez is already terminated (or is terminating), we won't receive
				 * any response. Do not treat such a case as an error - omit logging. */
				g_error_free(err);
				err = NULL;
			}
			else
				goto fail;
		}

	}

	debug("Closing BT: %d", t->bt_fd);

	ret = 0;
	t->release = NULL;
	t->state = TRANSPORT_IDLE;
	close(t->bt_fd);
	t->bt_fd = -1;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't release transport: %s", err->message);
		g_error_free(err);
	}
	return ret;
}

int transport_release_pcm(struct ba_transport *t) {

	/* disconnect transport from a client */
	t->pcm_client = -1;

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
