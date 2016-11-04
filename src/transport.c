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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <gio/gunixfdlist.h>

#include "a2dp-codecs.h"
#include "bluealsa.h"
#include "io.h"
#include "log.h"
#include "utils.h"


static int io_thread_create(struct ba_transport *t) {

	void *(*routine)(void *) = NULL;
	int ret;

	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		if (t->profile == BLUETOOTH_PROFILE_A2DP_SOURCE)
			switch (t->codec) {
			case A2DP_CODEC_SBC:
				routine = io_thread_a2dp_source_sbc;
				break;
#if 0
			case A2DP_CODEC_MPEG12:
				break;
#endif
#if ENABLE_AAC
			case A2DP_CODEC_MPEG24:
				routine = io_thread_a2dp_source_aac;
				break;
#endif
			default:
				warn("Codec not supported: %u", t->codec);
			}
		if (t->profile == BLUETOOTH_PROFILE_A2DP_SINK)
			switch (t->codec) {
			case A2DP_CODEC_SBC:
				routine = io_thread_a2dp_sink_sbc;
				break;
#if 0
			case A2DP_CODEC_MPEG12:
				break;
#endif
#if ENABLE_AAC
			case A2DP_CODEC_MPEG24:
				routine = io_thread_a2dp_sink_aac;
				break;
#endif
			default:
				warn("Codec not supported: %u", t->codec);
			}
		break;
	case TRANSPORT_TYPE_RFCOMM:
		routine = io_thread_rfcomm;
		break;
	case TRANSPORT_TYPE_SCO:
		break;
	}

	if (routine == NULL)
		return -1;

	if ((ret = pthread_create(&t->thread, NULL, routine, t)) != 0) {
		error("Couldn't create IO thread: %s", strerror(ret));
		t->thread = config.main_thread;
		return -1;
	}

	pthread_setname_np(t->thread, "baio");
	debug("Created new IO thread: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	return 0;
}

struct ba_device *device_new(int hci_dev_id, const bdaddr_t *addr, const char *name) {

	struct ba_device *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return NULL;

	d->hci_dev_id = hci_dev_id;
	bacpy(&d->addr, addr);

	strncpy(d->name, name, sizeof(d->name));
	d->name[sizeof(d->name) - 1] = '\0';

	d->transports = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)transport_free);

	return d;
}

void device_free(struct ba_device *d) {

	if (d == NULL)
		return;

	g_hash_table_unref(d->transports);
	free(d);
}

struct ba_device *device_get(GHashTable *devices, const char *key) {

	struct ba_device *d;
	char name[sizeof(d->name)];
	GVariant *property;
	bdaddr_t addr;

	if ((d = g_hash_table_lookup(devices, key)) != NULL)
		return d;

	g_dbus_device_path_to_bdaddr(key, &addr);
	ba2str(&addr, name);

	/* get local (user editable) Bluetooth device name */
	if ((property = g_dbus_get_property(config.dbus, "org.bluez", key,
					"org.bluez.Device1", "Alias")) != NULL) {
		strncpy(name, g_variant_get_string(property, NULL), sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
		g_variant_unref(property);
	}

	d = device_new(config.hci_dev.dev_id, &addr, name);
	g_hash_table_insert(devices, g_strdup(key), d);
	return d;
}

struct ba_device *device_lookup(GHashTable *devices, const char *key) {
	return g_hash_table_lookup(devices, key);
}

gboolean device_remove(GHashTable *devices, const char *key) {
	return g_hash_table_remove(devices, key);
}

struct ba_transport *transport_new(enum ba_transport_type type,
		const char *dbus_owner, const char *dbus_path,
		enum bluetooth_profile profile, uint8_t codec, const uint8_t *cconfig,
		size_t cconfig_size) {

	struct ba_transport *t;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		return NULL;

	t->type = type;

	t->dbus_owner = strdup(dbus_owner);
	t->dbus_path = strdup(dbus_path);

	t->profile = profile;
	t->codec = codec;
	t->volume = 127;

	if (cconfig_size > 0) {
		t->cconfig = malloc(cconfig_size);
		t->cconfig_size = cconfig_size;
		memcpy(t->cconfig, cconfig, cconfig_size);
	}

	pthread_mutex_init(&t->resume_mutex, NULL);
	pthread_cond_init(&t->resume, NULL);

	t->state = TRANSPORT_IDLE;
	t->thread = config.main_thread;
	t->bt_fd = -1;
	t->pcm.client = -1;
	t->pcm.fd = -1;

	return t;
}

void transport_free(struct ba_transport *t) {

	if (t == NULL)
		return;

	debug("Freeing transport: %s",
			bluetooth_profile_to_string(t->profile, t->codec));

	/* If the transport is active, prior to releasing resources, we have to
	 * terminate the IO thread (or at least make sure it is not running any
	 * more). Not doing so might result in an undefined behavior or even a
	 * race condition (closed and reused file descriptor). */
	if (!pthread_equal(t->thread, config.main_thread)) {
		pthread_cancel(t->thread);
		pthread_join(t->thread, NULL);
	}

	/* if possible, try to release resources gracefully */
	if (t->release != NULL)
		t->release(t);

	if (t->bt_fd != -1)
		close(t->bt_fd);

	transport_release_pcm(&t->pcm);

	pthread_mutex_destroy(&t->resume_mutex);
	pthread_cond_destroy(&t->resume);

	free(t->dbus_owner);
	free(t->dbus_path);
	free(t->cconfig);
	free(t);
}

struct ba_transport *transport_lookup(GHashTable *devices, const char *dbus_path) {

	GHashTableIter iter;
	struct ba_device *d;
	struct ba_transport *t;
	gpointer _key;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, &_key, (gpointer)&d)) {
		if ((t = g_hash_table_lookup(d->transports, dbus_path)) != NULL)
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
			if (t->pcm.client == client)
				return t;
	}

	return NULL;
}

gboolean transport_remove(GHashTable *devices, const char *dbus_path) {

	GHashTableIter iter;
	struct ba_device *d;
	gpointer _key;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, &_key, (gpointer)&d)) {
		if (g_hash_table_remove(d->transports, dbus_path)) {
			if (g_hash_table_size(d->transports) == 0)
				g_hash_table_iter_remove(&iter);
			return TRUE;
		}
	}

	return FALSE;
}

unsigned int transport_get_channels(const struct ba_transport *t) {

	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		switch (t->codec) {
		case A2DP_CODEC_SBC:
			switch (((a2dp_sbc_t *)t->cconfig)->channel_mode) {
			case SBC_CHANNEL_MODE_MONO:
				return 1;
			case SBC_CHANNEL_MODE_STEREO:
			case SBC_CHANNEL_MODE_JOINT_STEREO:
			case SBC_CHANNEL_MODE_DUAL_CHANNEL:
				return 2;
			}
			break;
		case A2DP_CODEC_MPEG12:
			switch (((a2dp_mpeg_t *)t->cconfig)->channel_mode) {
			case MPEG_CHANNEL_MODE_MONO:
				return 1;
			case MPEG_CHANNEL_MODE_STEREO:
			case MPEG_CHANNEL_MODE_JOINT_STEREO:
			case MPEG_CHANNEL_MODE_DUAL_CHANNEL:
				return 2;
			}
			break;
		case A2DP_CODEC_MPEG24:
			switch (((a2dp_aac_t *)t->cconfig)->channels) {
			case AAC_CHANNELS_1:
				return 1;
			case AAC_CHANNELS_2:
				return 2;
			}
			break;
		}
		break;
	case TRANSPORT_TYPE_RFCOMM:
		break;
	case TRANSPORT_TYPE_SCO:
		return 1;
	}

	/* the number of channels is unspecified */
	return 0;
}

unsigned int transport_get_sampling(const struct ba_transport *t) {

	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		switch (t->codec) {
		case A2DP_CODEC_SBC:
			switch (((a2dp_sbc_t *)t->cconfig)->frequency) {
			case SBC_SAMPLING_FREQ_16000:
				return 16000;
			case SBC_SAMPLING_FREQ_32000:
				return 32000;
			case SBC_SAMPLING_FREQ_44100:
				return 44100;
			case SBC_SAMPLING_FREQ_48000:
				return 48000;
			}
			break;
		case A2DP_CODEC_MPEG12:
			switch (((a2dp_mpeg_t *)t->cconfig)->frequency) {
			case MPEG_SAMPLING_FREQ_16000:
				return 16000;
			case MPEG_SAMPLING_FREQ_22050:
				return 22050;
			case MPEG_SAMPLING_FREQ_24000:
				return 24000;
			case MPEG_SAMPLING_FREQ_32000:
				return 32000;
			case MPEG_SAMPLING_FREQ_44100:
				return 44100;
			case MPEG_SAMPLING_FREQ_48000:
				return 48000;
			}
			break;
		case A2DP_CODEC_MPEG24:
			switch (AAC_GET_FREQUENCY(*(a2dp_aac_t *)t->cconfig)) {
			case AAC_SAMPLING_FREQ_8000:
				return 8000;
			case AAC_SAMPLING_FREQ_11025:
				return 11025;
			case AAC_SAMPLING_FREQ_12000:
				return 12000;
			case AAC_SAMPLING_FREQ_16000:
				return 16000;
			case AAC_SAMPLING_FREQ_22050:
				return 22050;
			case AAC_SAMPLING_FREQ_24000:
				return 24000;
			case AAC_SAMPLING_FREQ_32000:
				return 32000;
			case AAC_SAMPLING_FREQ_44100:
				return 44100;
			case AAC_SAMPLING_FREQ_48000:
				return 48000;
			case AAC_SAMPLING_FREQ_64000:
				return 64000;
			case AAC_SAMPLING_FREQ_88200:
				return 88200;
			case AAC_SAMPLING_FREQ_96000:
				return 96000;
			}
			break;
		}
		break;
	case TRANSPORT_TYPE_RFCOMM:
		break;
	case TRANSPORT_TYPE_SCO:
		return 8000;
	}

	/* the sampling frequency is unspecified */
	return 0;
}

int transport_set_state(struct ba_transport *t, enum ba_transport_state state) {
	debug("State transition: %d -> %d", t->state, state);

	if (t->state == state)
		return 0;

	const int created = !pthread_equal(t->thread, config.main_thread);
	int ret = 0;

	t->state = state;

	switch (state) {
	case TRANSPORT_IDLE:
		if (created) {
			pthread_cancel(t->thread);
			ret = pthread_join(t->thread, NULL);
			t->thread = config.main_thread;
		}
		break;
	case TRANSPORT_PENDING:
		ret = transport_acquire_bt(t);
		break;
	case TRANSPORT_ACTIVE:
	case TRANSPORT_PAUSED:
		if (!created)
			ret = io_thread_create(t);
		break;
	case TRANSPORT_ABORTED:
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

int transport_acquire_bt(struct ba_transport *t) {

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

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
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

int transport_acquire_bt_sco(struct ba_transport *t) {

	struct hci_dev_info di;

	if (hci_devinfo(t->device->hci_dev_id, &di) == -1) {
		error("Couldn't get HCI device info: %s", strerror(errno));
		return -1;
	}

	if ((t->bt_fd = hci_open_sco(&di, &t->device->addr)) == -1) {
		error("Couldn't open SCO link: %s", strerror(errno));
		return -1;
	}

	t->mtu_read = di.sco_mtu;
	t->mtu_write = di.sco_mtu;

	debug("New SCO link: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

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

	debug("Releasing transport: %s",
			bluetooth_profile_to_string(t->profile, t->codec));

	/* If the state is idle, it means that either transport was not acquired, or
	 * was released by the Bluez. In both cases there is no point in a explicit
	 * release request. It might even return error (e.g. not authorized). */
	if (t->state != TRANSPORT_IDLE) {

		msg = g_dbus_message_new_method_call(t->dbus_owner, t->dbus_path,
				"org.bluez.MediaTransport1", "Release");

		if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
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

int transport_release_bt_rfcomm(struct ba_transport *t) {

	if (t->bt_fd == -1)
		return 0;

	debug("Closing RFCOMM: %d", t->bt_fd);

	t->release = NULL;
	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

	/* BlueZ does not trigger profile disconnection signal when the Bluetooth
	 * link has been lost (e.g. device power down). However, it is required to
	 * remove transport from the transport pool before reconnecting. */
	if (t->state == TRANSPORT_ABORTED)
		g_hash_table_remove(t->device->transports, t->dbus_path);

	return 0;
}

int transport_release_pcm(struct ba_pcm *pcm) {

	int oldstate;

	/* Transport IO workers are managed using thread cancellation mechanism,
	 * so we have to take into account a possibility of cancellation during the
	 * execution. In this release function it is important to perform actions
	 * atomically. Since unlink and close calls are cancellation points, it is
	 * required to temporally disable cancellation. For a better understanding
	 * of what is going on, see the io_thread_read_pcm() function. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

	if (pcm->fifo != NULL) {
		debug("Cleaning PCM FIFO: %s", pcm->fifo);
		unlink(pcm->fifo);
		free(pcm->fifo);
		pcm->fifo = NULL;
	}

	if (pcm->fd != -1) {
		debug("Closing PCM: %d", pcm->fd);
		close(pcm->fd);
		pcm->fd = -1;
	}

	pthread_setcancelstate(oldstate, NULL);
	return 0;
}
