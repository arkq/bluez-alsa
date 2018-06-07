/*
 * BlueALSA - transport.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <gio/gunixfdlist.h>

#include "a2dp-codecs.h"
#include "bluealsa.h"
#include "ctl.h"
#include "hfp.h"
#include "io.h"
#include "rfcomm.h"
#include "utils.h"
#include "shared/log.h"


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
#if ENABLE_MP3
			case A2DP_CODEC_MPEG12:
				break;
#endif
#if ENABLE_AAC
			case A2DP_CODEC_MPEG24:
				routine = io_thread_a2dp_source_aac;
				break;
#endif
#if ENABLE_APTX
			case A2DP_CODEC_VENDOR_APTX:
				routine = io_thread_a2dp_source_aptx;
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
#if ENABLE_MP3
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
		routine = rfcomm_thread;
		break;
	case TRANSPORT_TYPE_SCO:
		routine = io_thread_sco;
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
			NULL, (GDestroyNotify)transport_free);

	return d;
}

void device_free(struct ba_device *d) {

	if (d == NULL)
		return;

	/* XXX: Modification-safe remove-all loop.
	 *
	 * By the usage of a standard g_hash_table_remove_all() function, one
	 * has to comply to the license warranty, which states that anything
	 * can happen. In our case it is true to the letter - SIGSEGV is 100%
	 * guaranteed.
	 *
	 * Our transport structure holds reference to some other transport
	 * structure within the same hash-table. Unfortunately, such a usage
	 * is not supported. Almost every GLib-2.0 function facilitates cache,
	 * which backfires at us if we modify hash-table from the inside of
	 * the destroy function. However, it is possible to "iterate" over
	 * a hash-table in a pop-like matter - reinitialize iterator after
	 * every modification. And voila - modification-safe remove loop. */
	for (;;) {

		GHashTableIter iter;
		struct ba_transport *t;

		g_hash_table_iter_init(&iter, d->transports);
		if (!g_hash_table_iter_next(&iter, NULL, (gpointer)&t))
			break;

		transport_free(t);
	}

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

bool device_remove(GHashTable *devices, const char *key) {
	return g_hash_table_remove(devices, key);
}

void device_set_battery_level(struct ba_device *d, uint8_t value) {
	d->battery.enabled = true;
	d->battery.level = value;
	bluealsa_ctl_event(BA_EVENT_UPDATE_BATTERY);
}

/**
 * Create new transport.
 *
 * @param device Pointer to the device structure.
 * @param type Transport type.
 * @param dbus_owner D-Bus service, which owns this transport.
 * @param dbus_path D-Bus service path for this transport.
 * @param profile Bluetooth profile.
 * @param codec Used audio codec.
 * @return On success, the pointer to the newly allocated transport structure
 *   is returned. If error occurs, NULL is returned and the errno variable is
 *   set to indicated the cause of the error. */
struct ba_transport *transport_new(
		struct ba_device *device,
		enum ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		enum bluetooth_profile profile,
		uint16_t codec) {

	struct ba_transport *t;
	int err;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		goto fail;

	t->device = device;
	t->type = type;

	t->profile = profile;
	t->codec = codec;

	/* HSP supports CVSD only */
	if (profile == BLUETOOTH_PROFILE_HSP_HS || profile == BLUETOOTH_PROFILE_HSP_AG)
		t->codec = HFP_CODEC_CVSD;

	t->state = TRANSPORT_IDLE;
	t->thread = config.main_thread;

	t->bt_fd = -1;
	t->sig_fd[0] = -1;
	t->sig_fd[1] = -1;

	if ((t->dbus_owner = strdup(dbus_owner)) == NULL)
		goto fail;
	if ((t->dbus_path = strdup(dbus_path)) == NULL)
		goto fail;

	if (pipe(t->sig_fd) == -1)
		goto fail;

	g_hash_table_insert(device->transports, t->dbus_path, t);
	return t;

fail:
	err = errno;
	transport_free(t);
	errno = err;
	return NULL;
}

struct ba_transport *transport_new_a2dp(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path,
		enum bluetooth_profile profile,
		uint16_t codec,
		const uint8_t *config,
		size_t config_size) {

	struct ba_transport *t;

	if ((t = transport_new(device, TRANSPORT_TYPE_A2DP,
					dbus_owner, dbus_path, profile, codec)) == NULL)
		return NULL;

	t->a2dp.ch1_volume = 127;
	t->a2dp.ch2_volume = 127;

	if (config_size > 0) {
		t->a2dp.cconfig = malloc(config_size);
		t->a2dp.cconfig_size = config_size;
		memcpy(t->a2dp.cconfig, config, config_size);
	}

	t->a2dp.pcm.fd = -1;
	t->a2dp.pcm.client = -1;
	pthread_cond_init(&t->a2dp.pcm.drained, NULL);
	pthread_mutex_init(&t->a2dp.pcm.drained_mn, NULL);

	bluealsa_ctl_event(BA_EVENT_TRANSPORT_ADDED);
	return t;
}

struct ba_transport *transport_new_rfcomm(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path,
		enum bluetooth_profile profile) {

	gchar *dbus_path_sco = NULL;
	struct ba_transport *t, *t_sco;

	if ((t = transport_new(device, TRANSPORT_TYPE_RFCOMM,
					dbus_owner, dbus_path, profile, -1)) == NULL)
		goto fail;

	dbus_path_sco = g_strdup_printf("%s/sco", dbus_path);
	if ((t_sco = transport_new(device, TRANSPORT_TYPE_SCO,
					dbus_owner, dbus_path_sco, profile, HFP_CODEC_UNDEFINED)) == NULL)
		goto fail;

	t->rfcomm.sco = t_sco;
	t_sco->sco.rfcomm = t;

	t_sco->sco.spk_gain = 15;
	t_sco->sco.mic_gain = 15;

	t_sco->sco.spk_pcm.fd = -1;
	t_sco->sco.spk_pcm.client = -1;
	pthread_cond_init(&t_sco->sco.spk_pcm.drained, NULL);
	pthread_mutex_init(&t_sco->sco.spk_pcm.drained_mn, NULL);

	t_sco->sco.mic_pcm.fd = -1;
	t_sco->sco.mic_pcm.client = -1;
	pthread_cond_init(&t_sco->sco.mic_pcm.drained, NULL);
	pthread_mutex_init(&t_sco->sco.mic_pcm.drained_mn, NULL);

	transport_set_state(t_sco, TRANSPORT_ACTIVE);

	bluealsa_ctl_event(BA_EVENT_TRANSPORT_ADDED);
	return t;

fail:
	if (dbus_path_sco != NULL)
		g_free(dbus_path_sco);
	transport_free(t);
	return NULL;
}

void transport_free(struct ba_transport *t) {

	if (t == NULL || t->state == TRANSPORT_LIMBO)
		return;

	t->state = TRANSPORT_LIMBO;
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
	if (t->sig_fd[0] != -1)
		close(t->sig_fd[0]);
	if (t->sig_fd[1] != -1)
		close(t->sig_fd[1]);

	/* free type-specific resources */
	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		transport_release_pcm(&t->a2dp.pcm);
		pthread_cond_destroy(&t->a2dp.pcm.drained);
		pthread_mutex_destroy(&t->a2dp.pcm.drained_mn);
		free(t->a2dp.cconfig);
		break;
	case TRANSPORT_TYPE_RFCOMM:
		memset(&t->device->battery, 0, sizeof(t->device->battery));
		memset(&t->device->xapl, 0, sizeof(t->device->xapl));
		transport_free(t->rfcomm.sco);
		break;
	case TRANSPORT_TYPE_SCO:
		transport_release_pcm(&t->sco.spk_pcm);
		pthread_cond_destroy(&t->sco.spk_pcm.drained);
		pthread_mutex_destroy(&t->sco.spk_pcm.drained_mn);
		transport_release_pcm(&t->sco.mic_pcm);
		pthread_cond_destroy(&t->sco.mic_pcm.drained);
		pthread_mutex_destroy(&t->sco.mic_pcm.drained_mn);
		t->sco.rfcomm->rfcomm.sco = NULL;
		break;
	}

	/* If the free action was called on the behalf of the destroy notification,
	 * removing a value from the hash-table shouldn't hurt - it would have been
	 * removed anyway. */
	g_hash_table_steal(t->device->transports, t->dbus_path);

	bluealsa_ctl_event(BA_EVENT_TRANSPORT_REMOVED);

	free(t->dbus_owner);
	free(t->dbus_path);
	free(t);
}

struct ba_transport *transport_lookup(GHashTable *devices, const char *dbus_path) {

	GHashTableIter iter;
	struct ba_device *d;
	struct ba_transport *t;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&d)) {
		if ((t = g_hash_table_lookup(d->transports, dbus_path)) != NULL)
			return t;
	}

	return NULL;
}

struct ba_transport *transport_lookup_pcm_client(GHashTable *devices, int client) {

	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t;

	g_hash_table_iter_init(&iter_d, devices);
	while (g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d)) {
		g_hash_table_iter_init(&iter_t, d->transports);
		while (g_hash_table_iter_next(&iter_t, NULL, (gpointer)&t)) {
			switch (t->type) {
			case TRANSPORT_TYPE_A2DP:
				if (t->a2dp.pcm.client == client)
					return t;
				break;
			case TRANSPORT_TYPE_RFCOMM:
				break;
			case TRANSPORT_TYPE_SCO:
				if (t->sco.spk_pcm.client == client)
					return t;
				if (t->sco.mic_pcm.client == client)
					return t;
				break;
			}
		}
	}

	return NULL;
}

bool transport_remove(GHashTable *devices, const char *dbus_path) {

	GHashTableIter iter;
	struct ba_device *d;
	struct ba_transport *t;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&d)) {
		/* Disassociate D-Bus owner before further actions. This will ensure,
		 * that we will not generate errors by using non-existent interface. */
		if ((t = g_hash_table_lookup(d->transports, dbus_path)) != NULL) {
			free(t->dbus_owner);
			t->dbus_owner = NULL;
		}
		if (g_hash_table_remove(d->transports, dbus_path)) {
			if (g_hash_table_size(d->transports) == 0)
				g_hash_table_iter_remove(&iter);
			return true;
		}
	}

	return false;
}

int transport_send_signal(struct ba_transport *t, enum ba_transport_signal sig) {
	return write(t->sig_fd[1], &sig, sizeof(sig));
}

int transport_send_rfcomm(struct ba_transport *t, const char command[32]) {

	char msg[sizeof(enum ba_transport_signal) + 32];

	((enum ba_transport_signal *)msg)[0] = TRANSPORT_SEND_RFCOMM;
	memcpy(&msg[sizeof(enum ba_transport_signal)], command, 32);

	return write(t->sig_fd[1], msg, sizeof(msg));
}

unsigned int transport_get_channels(const struct ba_transport *t) {

	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:
		switch (t->codec) {
		case A2DP_CODEC_SBC:
			switch (((a2dp_sbc_t *)t->a2dp.cconfig)->channel_mode) {
			case SBC_CHANNEL_MODE_MONO:
				return 1;
			case SBC_CHANNEL_MODE_STEREO:
			case SBC_CHANNEL_MODE_JOINT_STEREO:
			case SBC_CHANNEL_MODE_DUAL_CHANNEL:
				return 2;
			}
			break;
#if ENABLE_MP3
		case A2DP_CODEC_MPEG12:
			switch (((a2dp_mpeg_t *)t->a2dp.cconfig)->channel_mode) {
			case MPEG_CHANNEL_MODE_MONO:
				return 1;
			case MPEG_CHANNEL_MODE_STEREO:
			case MPEG_CHANNEL_MODE_JOINT_STEREO:
			case MPEG_CHANNEL_MODE_DUAL_CHANNEL:
				return 2;
			}
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			switch (((a2dp_aac_t *)t->a2dp.cconfig)->channels) {
			case AAC_CHANNELS_1:
				return 1;
			case AAC_CHANNELS_2:
				return 2;
			}
			break;
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			switch (((a2dp_aptx_t *)t->a2dp.cconfig)->channel_mode) {
			case APTX_CHANNEL_MODE_MONO:
				return 1;
			case APTX_CHANNEL_MODE_STEREO:
				return 2;
			}
			break;
#endif
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
			switch (((a2dp_sbc_t *)t->a2dp.cconfig)->frequency) {
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
#if ENABLE_MP3
		case A2DP_CODEC_MPEG12:
			switch (((a2dp_mpeg_t *)t->a2dp.cconfig)->frequency) {
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
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			switch (AAC_GET_FREQUENCY(*(a2dp_aac_t *)t->a2dp.cconfig)) {
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
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			switch (((a2dp_aptx_t *)t->a2dp.cconfig)->frequency) {
			case APTX_SAMPLING_FREQ_16000:
				return 16000;
			case APTX_SAMPLING_FREQ_32000:
				return 32000;
			case APTX_SAMPLING_FREQ_44100:
				return 44100;
			case APTX_SAMPLING_FREQ_48000:
				return 48000;
			}
			break;
#endif
		}
		break;
	case TRANSPORT_TYPE_RFCOMM:
		break;
	case TRANSPORT_TYPE_SCO:
		switch (t->codec) {
			case HFP_CODEC_CVSD:
				return 8000;
			case HFP_CODEC_MSBC:
				return 16000;
			default:
				debug("Unsupported SCO codec: %#x", t->codec);
		}
	}

	/* the sampling frequency is unspecified */
	return 0;
}

int transport_set_volume(struct ba_transport *t, uint8_t ch1_muted, uint8_t ch2_muted,
		uint8_t ch1_volume, uint8_t ch2_volume) {

	debug("Setting volume for %s profile %d: %d<>%d [%c%c]", batostr_(&t->device->addr),
			t->profile, ch1_volume, ch2_volume, ch1_muted ? 'M' : 'O', ch2_muted ? 'M' : 'O');

	switch (t->type) {
	case TRANSPORT_TYPE_A2DP:

		t->a2dp.ch1_muted = ch1_muted;
		t->a2dp.ch2_muted = ch2_muted;
		t->a2dp.ch1_volume = ch1_volume;
		t->a2dp.ch2_volume = ch2_volume;

		if (config.a2dp_volume) {
			uint16_t volume = (ch1_muted | ch2_muted) ? 0 : MIN(ch1_volume, ch2_volume);
			g_dbus_set_property(config.dbus, t->dbus_owner, t->dbus_path,
					"org.bluez.MediaTransport1", "Volume", g_variant_new_uint16(volume));
		}

		break;

	case TRANSPORT_TYPE_RFCOMM:
		break;

	case TRANSPORT_TYPE_SCO:

		t->sco.spk_muted = ch1_muted;
		t->sco.mic_muted = ch2_muted;
		t->sco.spk_gain = ch1_volume;
		t->sco.mic_gain = ch2_volume;

		/* notify associated RFCOMM transport */
		transport_send_signal(t->sco.rfcomm, TRANSPORT_SET_VOLUME);

		break;

	}

	return 0;
}

int transport_set_state(struct ba_transport *t, enum ba_transport_state state) {
	debug("State transition: %d -> %d", t->state, state);

	if (t->state == state)
		return 0;

	/* For the A2DP sink profile, the IO thread can not be created until the
	 * BT transport is acquired, otherwise thread initialized will fail. */
	if (t->profile == BLUETOOTH_PROFILE_A2DP_SINK &&
			t->state == TRANSPORT_IDLE && state != TRANSPORT_PENDING)
		return 0;

	const int created = !pthread_equal(t->thread, config.main_thread);
	int ret = 0;

	t->state = state;

	switch (state) {
	case TRANSPORT_IDLE:
		if (created) {
			pthread_cancel(t->thread);
			ret = pthread_join(t->thread, NULL);
		}
		break;
	case TRANSPORT_PENDING:
		/* When transport is marked as pending, try to acquire transport, but only
		 * if we are handing A2DP sink profile. For source profile, transport has
		 * to be acquired by our controller (during the PCM open request). */
		if (t->profile == BLUETOOTH_PROFILE_A2DP_SINK)
			ret = transport_acquire_bt_a2dp(t);
		break;
	case TRANSPORT_ACTIVE:
	case TRANSPORT_PAUSED:
		if (!created)
			ret = io_thread_create(t);
		break;
	case TRANSPORT_LIMBO:
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

int transport_drain_pcm(struct ba_transport *t) {

	struct ba_pcm *pcm = NULL;

	switch (t->profile) {
	case BLUETOOTH_PROFILE_NULL:
	case BLUETOOTH_PROFILE_A2DP_SINK:
		break;
	case BLUETOOTH_PROFILE_A2DP_SOURCE:
		pcm = &t->a2dp.pcm;
		break;
	case BLUETOOTH_PROFILE_HSP_AG:
	case BLUETOOTH_PROFILE_HFP_AG:
		pcm = &t->sco.spk_pcm;
		break;
	case BLUETOOTH_PROFILE_HSP_HS:
	case BLUETOOTH_PROFILE_HFP_HF:
		break;
	}

	if (pcm == NULL || t->state != TRANSPORT_ACTIVE)
		return 0;

	pthread_mutex_lock(&pcm->drained_mn);

	transport_send_signal(t, TRANSPORT_PCM_SYNC);
	pthread_cond_wait(&pcm->drained, &pcm->drained_mn);

	pthread_mutex_unlock(&pcm->drained_mn);

	/* TODO: Asynchronous transport release.
	 *
	 * Unfortunately, BlueZ does not provide API for internal buffer drain.
	 * Also, there is no specification for Bluetooth playback drain. In order
	 * to make sure, that all samples are played out, we have to wait some
	 * arbitrary time before releasing transport. In order to make it right,
	 * there is a requirement for an asynchronous release mechanism, which
	 * is not implemented - it requires a little bit of refactoring. */
	usleep(200000);

	debug("PCM drained");
	return 0;
}

int transport_acquire_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg, *rep;
	GUnixFDList *fd_list;
	GError *err = NULL;

	if (t->bt_fd != -1) {
		warn("Closing dangling BT socket: %d", t->bt_fd);
		close(t->bt_fd);
		t->bt_fd = -1;
	}

	msg = g_dbus_message_new_method_call(t->dbus_owner, t->dbus_path, "org.bluez.MediaTransport1",
			t->state == TRANSPORT_PENDING ? "TryAcquire" : "Acquire");

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
	t->release = transport_release_bt_a2dp;

	/* Minimize audio delay and increase responsiveness (seeking, stopping) by
	 * decreasing the BT socket output buffer. We will use a tripled write MTU
	 * value, in order to prevent tearing due to temporal heavy load. */
	size_t size = t->mtu_write * 3;
	if (setsockopt(t->bt_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1)
		warn("Couldn't set socket output buffer size: %s", strerror(errno));

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

int transport_release_bt_a2dp(struct ba_transport *t) {

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
	 * was released by the BlueZ. In both cases there is no point in a explicit
	 * release request. It might even return error (e.g. not authorized). */
	if (t->state != TRANSPORT_IDLE && t->dbus_owner != NULL) {

		msg = g_dbus_message_new_method_call(t->dbus_owner, t->dbus_path,
				"org.bluez.MediaTransport1", "Release");

		if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
						G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
			goto fail;

		if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
			g_dbus_message_to_gerror(rep, &err);
			if (err->code == G_DBUS_ERROR_NO_REPLY) {
				/* If BlueZ is already terminated (or is terminating), we won't receive
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
	transport_free(t);

	return 0;
}

int transport_acquire_bt_sco(struct ba_transport *t) {

	struct hci_dev_info di;

	if (t->bt_fd != -1)
		return t->bt_fd;

	if (hci_devinfo(t->device->hci_dev_id, &di) == -1) {
		error("Couldn't get HCI device info: %s", strerror(errno));
		return -1;
	}

	if ((t->bt_fd = hci_open_sco(&di, &t->device->addr, t->codec != HFP_CODEC_CVSD)) == -1) {
		error("Couldn't open SCO link: %s", strerror(errno));
		return -1;
	}

	t->mtu_read = di.sco_mtu;
	t->mtu_write = di.sco_mtu;
	t->release = transport_release_bt_sco;

	/* XXX: It seems, that the MTU values returned by the HCI interface
	 *      are incorrect (or our interpretation of them is incorrect). */
	t->mtu_read = 48;
	t->mtu_write = 48;

	debug("New SCO link: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

	return t->bt_fd;
}

int transport_release_bt_sco(struct ba_transport *t) {

	if (t->bt_fd == -1)
		return 0;

	debug("Closing SCO: %d", t->bt_fd);

	t->release = NULL;
	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

	return 0;
}

int transport_release_pcm(struct ba_pcm *pcm) {

	int oldstate;

	/* Transport IO workers are managed using thread cancellation mechanism,
	 * so we have to take into account a possibility of cancellation during the
	 * execution. In this release function it is important to perform actions
	 * atomically. Since close call is a cancellation point, it is required to
	 * temporally disable cancellation. For a better understanding of what is
	 * going on, see the io_thread_read_pcm() function. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

	if (pcm->fd != -1) {
		debug("Closing PCM: %d", pcm->fd);
		close(pcm->fd);
		pcm->fd = -1;
	}

	pthread_setcancelstate(oldstate, NULL);
	return 0;
}

/**
 * Wrapper for release callback, which can be used by the pthread cleanup. */
void transport_pthread_cleanup(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	/* During the normal operation mode, the release callback should not
	 * be NULL. Hence, we will relay on this callback - file descriptors
	 * are closed in it. */
	if (t->release != NULL)
		t->release(t);

	/* Make sure, that after termination, this thread handler will not
	 * be used anymore. */
	t->thread = config.main_thread;

	/* XXX: If the order of the cleanup push is right, this function will
	 *      indicate the end of the IO/RFCOMM thread. */
	debug("Exiting IO thread");
}
