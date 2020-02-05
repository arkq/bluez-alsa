/*
 * BlueALSA - ba-transport.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ba-transport.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "a2dp.h"
#include "a2dp-codecs.h"
#include "ba-adapter.h"
#include "bluealsa.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "hci.h"
#include "hfp.h"
#include "rfcomm.h"
#include "sco.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/log.h"

/**
 * Create new transport.
 *
 * @param device Pointer to the device structure.
 * @param dbus_owner D-Bus service, which owns this transport.
 * @param dbus_path D-Bus service path for this transport.
 * @return On success, the pointer to the newly allocated transport structure
 *   is returned. If error occurs, NULL is returned and the errno variable is
 *   set to indicated the cause of the error. */
struct ba_transport *ba_transport_new(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t;
	int err;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		return NULL;

	t->d = ba_device_ref(device);
	t->type.profile = BA_TRANSPORT_PROFILE_NONE;
	t->ref_count = 1;

	pthread_mutex_init(&t->mutex, NULL);

	t->state = BA_TRANSPORT_STATE_IDLE;
	t->thread = config.main_thread;

	t->bt_fd = -1;
	t->sig_fd[0] = -1;
	t->sig_fd[1] = -1;

	if ((t->bluez_dbus_owner = strdup(dbus_owner)) == NULL)
		goto fail;
	if ((t->bluez_dbus_path = strdup(dbus_path)) == NULL)
		goto fail;

	if (pipe(t->sig_fd) == -1)
		goto fail;

	pthread_mutex_lock(&device->transports_mutex);
	g_hash_table_insert(device->transports, t->bluez_dbus_path, t);
	pthread_mutex_unlock(&device->transports_mutex);

	return t;

fail:
	err = errno;
	ba_transport_unref(t);
	errno = err;
	return NULL;
}

/* These acquire/release helper functions should be defined before the
 * corresponding ba_transport_new_* ones. However, git commit history is
 * more important, so we're going to keep these functions at original
 * locations and use forward declarations instead. */
static int transport_acquire_bt_a2dp(struct ba_transport *t);
static int transport_release_bt_a2dp(struct ba_transport *t);
static int transport_release_bt_rfcomm(struct ba_transport *t);
static int transport_acquire_bt_sco(struct ba_transport *t);
static int transport_release_bt_sco(struct ba_transport *t);

struct ba_transport *ba_transport_new_a2dp(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		const void *cconfig,
		size_t cconfig_size) {

	struct ba_transport *t;

	if ((t = ba_transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->type = type;

	t->a2dp.pcm.volume[0].level = 127;
	t->a2dp.pcm.volume[1].level = 127;

	if (cconfig_size > 0) {
		t->a2dp.cconfig = g_memdup(cconfig, cconfig_size);
		t->a2dp.cconfig_size = cconfig_size;
	}

	t->a2dp.pcm.t = t;
	t->a2dp.pcm.fd = -1;
	t->a2dp.pcm.client = -1;

	t->a2dp.pcm.mode = BA_TRANSPORT_PCM_MODE_SOURCE;
	if (type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		t->a2dp.pcm.mode = BA_TRANSPORT_PCM_MODE_SINK;

	pthread_mutex_init(&t->a2dp.drained_mtx, NULL);
	pthread_cond_init(&t->a2dp.drained, NULL);

	t->acquire = transport_acquire_bt_a2dp;
	t->release = transport_release_bt_a2dp;

	ba_transport_update_codec(t, type.codec);

	t->a2dp.pcm.ba_dbus_path = g_strdup_printf("%s/a2dp/%s", device->ba_dbus_path,
			t->a2dp.pcm.mode == BA_TRANSPORT_PCM_MODE_SOURCE ? "source" : "sink");
	bluealsa_dbus_pcm_register(&t->a2dp.pcm, NULL);

	return t;
}

struct ba_transport *ba_transport_new_rfcomm(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t, *t_sco;
	char dbus_path_sco[64];
	int err;

	if ((t = ba_transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->type.profile = type.profile | BA_TRANSPORT_PROFILE_RFCOMM;
	t->rfcomm.handler_fd = -1;

	snprintf(dbus_path_sco, sizeof(dbus_path_sco), "%s/sco", dbus_path);
	if ((t_sco = ba_transport_new_sco(device, type, dbus_owner, dbus_path_sco, t)) == NULL)
		goto fail;

	pthread_mutex_init(&t->rfcomm.codec_selection_completed_mtx, NULL);
	pthread_cond_init(&t->rfcomm.codec_selection_completed, NULL);

	t->rfcomm.sco = t_sco;
	t->release = transport_release_bt_rfcomm;

	t->rfcomm.ba_dbus_path = g_strdup_printf("%s/rfcomm", device->ba_dbus_path);
	bluealsa_dbus_rfcomm_register(t, NULL);

	return t;

fail:
	err = errno;
	ba_transport_unref(t);
	errno = err;
	return NULL;
}

struct ba_transport *ba_transport_new_sco(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		struct ba_transport *rfcomm) {

	struct ba_transport *t;

	if ((t = ba_transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	/* HSP supports CVSD only */
	if (type.profile & BA_TRANSPORT_PROFILE_MASK_HSP)
		type.codec = HFP_CODEC_CVSD;

#if ENABLE_MSBC
	/* Check whether support for codec other than
	 * CVSD is possible with underlying adapter. */
	if (!BA_TEST_ESCO_SUPPORT(device->a))
		type.codec = HFP_CODEC_CVSD;
#else
	type.codec = HFP_CODEC_CVSD;
#endif

	t->type = type;

	if (rfcomm != NULL)
		t->sco.rfcomm = ba_transport_ref(rfcomm);

	t->sco.spk_pcm.volume[0].level = 15;
	t->sco.mic_pcm.volume[0].level = 15;

	t->sco.spk_pcm.t = t;
	t->sco.spk_pcm.fd = -1;
	t->sco.spk_pcm.client = -1;
	t->sco.spk_pcm.mode = BA_TRANSPORT_PCM_MODE_SINK;

	t->sco.mic_pcm.t = t;
	t->sco.mic_pcm.fd = -1;
	t->sco.mic_pcm.client = -1;
	t->sco.mic_pcm.mode = BA_TRANSPORT_PCM_MODE_SOURCE;

	pthread_mutex_init(&t->sco.spk_drained_mtx, NULL);
	pthread_cond_init(&t->sco.spk_drained, NULL);

	t->acquire = transport_acquire_bt_sco;
	t->release = transport_release_bt_sco;

	ba_transport_update_codec(t, type.codec);

	t->sco.spk_pcm.ba_dbus_path = g_strdup_printf("%s/sco/sink", device->ba_dbus_path);
	bluealsa_dbus_pcm_register(&t->sco.spk_pcm, NULL);

	t->sco.mic_pcm.ba_dbus_path = g_strdup_printf("%s/sco/source", device->ba_dbus_path);
	bluealsa_dbus_pcm_register(&t->sco.mic_pcm, NULL);

	return t;
}

struct ba_transport *ba_transport_lookup(
		struct ba_device *device,
		const char *dbus_path) {

	struct ba_transport *t;

	pthread_mutex_lock(&device->transports_mutex);
	if ((t = g_hash_table_lookup(device->transports, dbus_path)) != NULL)
		t->ref_count++;
	pthread_mutex_unlock(&device->transports_mutex);

	return t;
}

struct ba_transport *ba_transport_ref(
		struct ba_transport *t) {

	struct ba_device *d = t->d;

	pthread_mutex_lock(&d->transports_mutex);
	t->ref_count++;
	pthread_mutex_unlock(&d->transports_mutex);

	return t;
}

/**
 * Synchronous transport thread cancellation. */
static void ba_transport_pthread_cancel(struct ba_transport *t) {

	if (pthread_equal(t->thread, config.main_thread) ||
			pthread_equal(t->thread, pthread_self()))
		return;

	int err;
	if ((err = pthread_cancel(t->thread)) != 0 && err != ESRCH)
		warn("Couldn't cancel transport thread: %s", strerror(err));
	if ((err = pthread_join(t->thread, NULL)) != 0)
		warn("Couldn't join transport thread: %s", strerror(err));

	/* Indicate that the thread has been successfully terminated. Also, make sure,
	 * that after termination, this thread handler will not be used anymore. */
	t->thread = config.main_thread;

}

void ba_transport_destroy(struct ba_transport *t) {

	/* Remove D-Bus interfaces, so no one will access
	 * this transport during the destroy procedure. */
	if (t->type.profile & BA_TRANSPORT_PROFILE_RFCOMM)
		bluealsa_dbus_rfcomm_unregister(t);
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		bluealsa_dbus_pcm_unregister(&t->a2dp.pcm);
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		bluealsa_dbus_pcm_unregister(&t->sco.spk_pcm);
		bluealsa_dbus_pcm_unregister(&t->sco.mic_pcm);
	}

	/* If the transport is active, prior to releasing resources, we have to
	 * terminate the IO thread (or at least make sure it is not running any
	 * more). Not doing so might result in an undefined behavior or even a
	 * race condition (closed and reused file descriptor). */
	ba_transport_pthread_cancel(t);

	/* if possible, try to release resources gracefully */
	if (t->release != NULL)
		t->release(t);

	ba_transport_unref(t);
}

void ba_transport_unref(struct ba_transport *t) {

	int ref_count;
	struct ba_device *d = t->d;

	pthread_mutex_lock(&d->transports_mutex);
	if ((ref_count = --t->ref_count) == 0)
		/* detach transport from the device */
		g_hash_table_steal(d->transports, t->bluez_dbus_path);
	pthread_mutex_unlock(&d->transports_mutex);

	if (ref_count > 0)
		return;

	debug("Freeing transport: %s", ba_transport_type_to_string(t->type));

	if (t->bt_fd != -1)
		close(t->bt_fd);
	if (t->sig_fd[0] != -1)
		close(t->sig_fd[0]);
	if (t->sig_fd[1] != -1)
		close(t->sig_fd[1]);

	ba_device_unref(d);

	if (t->type.profile & BA_TRANSPORT_PROFILE_RFCOMM) {
		if (t->rfcomm.sco != NULL)
			ba_transport_unref(t->rfcomm.sco);
		if (t->rfcomm.handler_fd != -1)
			close(t->rfcomm.handler_fd);
		if (t->rfcomm.ba_dbus_path != NULL)
			g_free(t->rfcomm.ba_dbus_path);
		pthread_mutex_destroy(&t->rfcomm.codec_selection_completed_mtx);
		pthread_cond_destroy(&t->rfcomm.codec_selection_completed);
		d->battery_level = -1;
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		pthread_mutex_destroy(&t->sco.spk_drained_mtx);
		pthread_cond_destroy(&t->sco.spk_drained);
		ba_transport_release_pcm(&t->sco.spk_pcm);
		ba_transport_release_pcm(&t->sco.mic_pcm);
		if (t->sco.rfcomm != NULL)
			ba_transport_unref(t->sco.rfcomm);
		if (t->sco.spk_pcm.ba_dbus_path != NULL)
			g_free(t->sco.spk_pcm.ba_dbus_path);
		if (t->sco.mic_pcm.ba_dbus_path != NULL)
			g_free(t->sco.mic_pcm.ba_dbus_path);
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		ba_transport_release_pcm(&t->a2dp.pcm);
		pthread_mutex_destroy(&t->a2dp.drained_mtx);
		pthread_cond_destroy(&t->a2dp.drained);
		if (t->a2dp.pcm.ba_dbus_path != NULL)
			g_free(t->a2dp.pcm.ba_dbus_path);
		free(t->a2dp.cconfig);
	}

	pthread_mutex_destroy(&t->mutex);
	free(t->bluez_dbus_owner);
	free(t->bluez_dbus_path);
	free(t);
}

int ba_transport_send_signal(struct ba_transport *t, enum ba_transport_signal sig) {
	return write(t->sig_fd[1], &sig, sizeof(sig));
}

enum ba_transport_signal ba_transport_recv_signal(struct ba_transport *t) {

	enum ba_transport_signal sig;
	ssize_t ret;

	while ((ret = read(t->sig_fd[0], &sig, sizeof(sig))) == -1 &&
			errno == EINTR)
		continue;

	if (ret != sizeof(sig)) {
		warn("Couldn't read transport signal: %s", strerror(errno));
		return BA_TRANSPORT_SIGNAL_PING;
	}

	return sig;
}

int ba_transport_select_codec(
		struct ba_transport *t,
		uint16_t codec) {

	switch (t->type.profile) {
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HFP_AG:
#if ENABLE_MSBC

		/* codec already selected, skip switching */
		if (t->type.codec == codec)
			return 0;

		/* we have no access to RFCOMM with oFono back-end */
		if (t->sco.ofono)
			return errno = ENOTSUP, -1;

		struct ba_transport * const t_rfcomm = t->sco.rfcomm;
		pthread_mutex_lock(&t_rfcomm->rfcomm.codec_selection_completed_mtx);

		/* release ongoing connection */
		ba_transport_release_pcm(&t->sco.spk_pcm);
		ba_transport_release_pcm(&t->sco.mic_pcm);
		t->release(t);

		switch (codec) {
		case HFP_CODEC_CVSD:
			ba_transport_send_signal(t_rfcomm, BA_TRANSPORT_SIGNAL_HFP_SET_CODEC_CVSD);
			pthread_cond_wait(&t_rfcomm->rfcomm.codec_selection_completed,
					&t_rfcomm->rfcomm.codec_selection_completed_mtx);
			break;
		case HFP_CODEC_MSBC:
			ba_transport_send_signal(t_rfcomm, BA_TRANSPORT_SIGNAL_HFP_SET_CODEC_MSBC);
			pthread_cond_wait(&t_rfcomm->rfcomm.codec_selection_completed,
					&t_rfcomm->rfcomm.codec_selection_completed_mtx);
			break;
		}

		pthread_mutex_unlock(&t_rfcomm->rfcomm.codec_selection_completed_mtx);
		if (t->type.codec != codec)
			return errno = EIO, -1;

		break;
#endif

	case BA_TRANSPORT_PROFILE_HSP_HS:
	case BA_TRANSPORT_PROFILE_HSP_AG:
	default:
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static void transport_update_format(struct ba_transport *t) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16LE;
	}

	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile)) {
		t->sco.spk_pcm.format = BA_TRANSPORT_PCM_FORMAT_S16LE;
		t->sco.mic_pcm.format = BA_TRANSPORT_PCM_FORMAT_S16LE;
	}

}

static void transport_update_channels(struct ba_transport *t) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			switch (((a2dp_sbc_t *)t->a2dp.cconfig)->channel_mode) {
			case SBC_CHANNEL_MODE_MONO:
				t->a2dp.pcm.channels = 1;
				return;
			case SBC_CHANNEL_MODE_STEREO:
			case SBC_CHANNEL_MODE_JOINT_STEREO:
			case SBC_CHANNEL_MODE_DUAL_CHANNEL:
				t->a2dp.pcm.channels = 2;
				return;
			default:
				debug("Invalid channel mode: %#x",
						((a2dp_sbc_t *)t->a2dp.cconfig)->channel_mode);
				t->a2dp.pcm.channels = 0;
				return;
			}
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			switch (((a2dp_mpeg_t *)t->a2dp.cconfig)->channel_mode) {
			case MPEG_CHANNEL_MODE_MONO:
				t->a2dp.pcm.channels = 1;
				return;
			case MPEG_CHANNEL_MODE_STEREO:
			case MPEG_CHANNEL_MODE_JOINT_STEREO:
			case MPEG_CHANNEL_MODE_DUAL_CHANNEL:
				t->a2dp.pcm.channels = 2;
				return;
			default:
				debug("Invalid channel mode: %#x",
						((a2dp_mpeg_t *)t->a2dp.cconfig)->channel_mode);
				t->a2dp.pcm.channels = 0;
				return;
			}
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			switch (((a2dp_aac_t *)t->a2dp.cconfig)->channels) {
			case AAC_CHANNELS_1:
				t->a2dp.pcm.channels = 1;
				return;
			case AAC_CHANNELS_2:
				t->a2dp.pcm.channels = 2;
				return;
			default:
				debug("Invalid channel mode: %#x",
						((a2dp_aac_t *)t->a2dp.cconfig)->channels);
				t->a2dp.pcm.channels = 0;
				return;
			}
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			switch (((a2dp_aptx_t *)t->a2dp.cconfig)->channel_mode) {
			case APTX_CHANNEL_MODE_MONO:
				t->a2dp.pcm.channels = 1;
				return;
			case APTX_CHANNEL_MODE_STEREO:
				t->a2dp.pcm.channels = 2;
				return;
			default:
				debug("Invalid channel mode: %#x",
						((a2dp_aptx_t *)t->a2dp.cconfig)->channel_mode);
				t->a2dp.pcm.channels = 0;
				return;
			}
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			switch (((a2dp_aptx_hd_t *)t->a2dp.cconfig)->aptx.channel_mode) {
			case APTX_CHANNEL_MODE_MONO:
				t->a2dp.pcm.channels = 1;
				return;
			case APTX_CHANNEL_MODE_STEREO:
				t->a2dp.pcm.channels = 2;
				return;
			default:
				debug("Invalid channel mode: %#x",
						((a2dp_aptx_hd_t *)t->a2dp.cconfig)->aptx.channel_mode);
				t->a2dp.pcm.channels = 0;
				return;
			}
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			switch (((a2dp_ldac_t *)t->a2dp.cconfig)->channel_mode) {
			case LDAC_CHANNEL_MODE_MONO:
				t->a2dp.pcm.channels = 1;
				return;
			case LDAC_CHANNEL_MODE_STEREO:
			case LDAC_CHANNEL_MODE_DUAL:
				t->a2dp.pcm.channels = 2;
				return;
			default:
				debug("Invalid channel mode: %#x",
						((a2dp_ldac_t *)t->a2dp.cconfig)->channel_mode);
				t->a2dp.pcm.channels = 0;
				return;
			}
#endif
		default:
			warn("Unsupported A2DP codec: %#x", t->type.codec);
			t->a2dp.pcm.channels = 0;
			return;
		}

	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile)) {
		t->sco.spk_pcm.channels = 1;
		t->sco.mic_pcm.channels = 1;
	}

}

static void transport_update_sampling(struct ba_transport *t) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			switch (((a2dp_sbc_t *)t->a2dp.cconfig)->frequency) {
			case SBC_SAMPLING_FREQ_16000:
				t->a2dp.pcm.sampling = 16000;
				return;
			case SBC_SAMPLING_FREQ_32000:
				t->a2dp.pcm.sampling = 32000;
				return;
			case SBC_SAMPLING_FREQ_44100:
				t->a2dp.pcm.sampling = 44100;
				return;
			case SBC_SAMPLING_FREQ_48000:
				t->a2dp.pcm.sampling = 48000;
				return;
			default:
				debug("Invalid sampling frequency: %#x",
						((a2dp_sbc_t *)t->a2dp.cconfig)->frequency);
				t->a2dp.pcm.sampling = 0;
				return;
			}
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			switch (((a2dp_mpeg_t *)t->a2dp.cconfig)->frequency) {
			case MPEG_SAMPLING_FREQ_16000:
				t->a2dp.pcm.sampling = 16000;
				return;
			case MPEG_SAMPLING_FREQ_22050:
				t->a2dp.pcm.sampling = 22050;
				return;
			case MPEG_SAMPLING_FREQ_24000:
				t->a2dp.pcm.sampling = 24000;
				return;
			case MPEG_SAMPLING_FREQ_32000:
				t->a2dp.pcm.sampling = 32000;
				return;
			case MPEG_SAMPLING_FREQ_44100:
				t->a2dp.pcm.sampling = 44100;
				return;
			case MPEG_SAMPLING_FREQ_48000:
				t->a2dp.pcm.sampling = 48000;
				return;
			default:
				debug("Invalid sampling frequency: %#x",
						((a2dp_mpeg_t *)t->a2dp.cconfig)->frequency);
				t->a2dp.pcm.sampling = 0;
				return;
			}
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			switch (AAC_GET_FREQUENCY(*(a2dp_aac_t *)t->a2dp.cconfig)) {
			case AAC_SAMPLING_FREQ_8000:
				t->a2dp.pcm.sampling = 8000;
				return;
			case AAC_SAMPLING_FREQ_11025:
				t->a2dp.pcm.sampling = 11025;
				return;
			case AAC_SAMPLING_FREQ_12000:
				t->a2dp.pcm.sampling = 12000;
				return;
			case AAC_SAMPLING_FREQ_16000:
				t->a2dp.pcm.sampling = 16000;
				return;
			case AAC_SAMPLING_FREQ_22050:
				t->a2dp.pcm.sampling = 22050;
				return;
			case AAC_SAMPLING_FREQ_24000:
				t->a2dp.pcm.sampling = 24000;
				return;
			case AAC_SAMPLING_FREQ_32000:
				t->a2dp.pcm.sampling = 32000;
				return;
			case AAC_SAMPLING_FREQ_44100:
				t->a2dp.pcm.sampling = 44100;
				return;
			case AAC_SAMPLING_FREQ_48000:
				t->a2dp.pcm.sampling = 48000;
				return;
			case AAC_SAMPLING_FREQ_64000:
				t->a2dp.pcm.sampling = 64000;
				return;
			case AAC_SAMPLING_FREQ_88200:
				t->a2dp.pcm.sampling = 88200;
				return;
			case AAC_SAMPLING_FREQ_96000:
				t->a2dp.pcm.sampling = 96000;
				return;
			default:
				debug("Invalid sampling frequency: %#x",
						AAC_GET_FREQUENCY(*(a2dp_aac_t *)t->a2dp.cconfig));
				t->a2dp.pcm.sampling = 0;
				return;
			}
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			switch (((a2dp_aptx_t *)t->a2dp.cconfig)->frequency) {
			case APTX_SAMPLING_FREQ_16000:
				t->a2dp.pcm.sampling = 16000;
				return;
			case APTX_SAMPLING_FREQ_32000:
				t->a2dp.pcm.sampling = 32000;
				return;
			case APTX_SAMPLING_FREQ_44100:
				t->a2dp.pcm.sampling = 44100;
				return;
			case APTX_SAMPLING_FREQ_48000:
				t->a2dp.pcm.sampling = 48000;
				return;
			default:
				debug("Invalid sampling frequency: %#x",
						((a2dp_aptx_t *)t->a2dp.cconfig)->frequency);
				t->a2dp.pcm.sampling = 0;
				return;
			}
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			switch (((a2dp_aptx_hd_t *)t->a2dp.cconfig)->aptx.frequency) {
			case APTX_SAMPLING_FREQ_16000:
				t->a2dp.pcm.sampling = 16000;
				return;
			case APTX_SAMPLING_FREQ_32000:
				t->a2dp.pcm.sampling = 32000;
				return;
			case APTX_SAMPLING_FREQ_44100:
				t->a2dp.pcm.sampling = 44100;
				return;
			case APTX_SAMPLING_FREQ_48000:
				t->a2dp.pcm.sampling = 48000;
				return;
			default:
				debug("Invalid sampling frequency: %#x",
						((a2dp_aptx_hd_t *)t->a2dp.cconfig)->aptx.frequency);
				t->a2dp.pcm.sampling = 0;
				return;
			}
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			switch (((a2dp_ldac_t *)t->a2dp.cconfig)->frequency) {
			case LDAC_SAMPLING_FREQ_44100:
				t->a2dp.pcm.sampling = 44100;
				return;
			case LDAC_SAMPLING_FREQ_48000:
				t->a2dp.pcm.sampling = 48000;
				return;
			case LDAC_SAMPLING_FREQ_88200:
				t->a2dp.pcm.sampling = 88200;
				return;
			case LDAC_SAMPLING_FREQ_96000:
				t->a2dp.pcm.sampling = 96000;
				return;
			case LDAC_SAMPLING_FREQ_176400:
				t->a2dp.pcm.sampling = 176400;
				return;
			case LDAC_SAMPLING_FREQ_192000:
				t->a2dp.pcm.sampling = 192000;
				return;
			default:
				debug("Invalid sampling frequency: %#x",
						((a2dp_ldac_t *)t->a2dp.cconfig)->frequency);
				t->a2dp.pcm.sampling = 0;
				return;
			}
#endif
		default:
			warn("Unsupported A2DP codec: %#x", t->type.codec);
			t->a2dp.pcm.sampling = 0;
			return;
		}

	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		switch (t->type.codec) {
		case HFP_CODEC_CVSD:
			t->sco.spk_pcm.sampling = 8000;
			t->sco.mic_pcm.sampling = 8000;
			return;
		case HFP_CODEC_MSBC:
			t->sco.spk_pcm.sampling = 16000;
			t->sco.mic_pcm.sampling = 16000;
			return;
		default:
			debug("Unsupported SCO codec: %#x", t->type.codec);
			/* fall-through */
		case HFP_CODEC_UNDEFINED:
			t->sco.spk_pcm.sampling = 0;
			t->sco.mic_pcm.sampling = 0;
		}

}

void ba_transport_update_codec(
		struct ba_transport *t,
		uint16_t codec) {

	t->type.codec = codec;

	transport_update_format(t);
	transport_update_channels(t);
	transport_update_sampling(t);

}

uint16_t ba_transport_get_delay(const struct ba_transport *t) {
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return t->a2dp.delay + t->a2dp.pcm.delay;
	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		return t->sco.spk_pcm.delay + 10;
	return 0;
}

/**
 * Get transport PCM volume encoded as a single 16-bit value. */
uint16_t ba_transport_pcm_get_volume_packed(const struct ba_transport_pcm *pcm) {
	return
		((pcm->volume[0].muted << 7) | pcm->volume[0].level) << 8 |
		((pcm->volume[1].muted << 7) | pcm->volume[1].level);
}

/**
 * Set transport PCM volume from an encoded single 16-bit value. */
int ba_transport_pcm_set_volume_packed(struct ba_transport_pcm *pcm, uint16_t value) {

	const struct ba_transport *t = pcm->t;
	uint8_t ch1 = value >> 8;
	uint8_t ch2 = value & 0xFF;

	debug("Setting volume: %d<>%d [%c%c]", ch1 & 0x7F, ch2 & 0x7F,
			ch1 & 0x80 ? 'M' : 'O', ch2 & 0x80 ? 'M' : 'O');

	pcm->volume[0].muted = !!(ch1 & 0x80);
	pcm->volume[1].muted = !!(ch2 & 0x80);
	pcm->volume[0].level = ch1 & 0x7F;
	pcm->volume[1].level = ch2 & 0x7F;

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP &&
			config.a2dp.volume) {

		uint16_t volume = 0;
		if (!t->a2dp.pcm.volume[0].muted && !t->a2dp.pcm.volume[1].muted)
			volume = (t->a2dp.pcm.volume[0].level + t->a2dp.pcm.volume[1].level) / 2;

		GError *err = NULL;
		g_dbus_set_property(config.dbus, t->bluez_dbus_owner, t->bluez_dbus_path,
				BLUEZ_IFACE_MEDIA_TRANSPORT, "Volume", g_variant_new_uint16(volume), &err);

		if (err != NULL) {
			warn("Couldn't set BT device volume: %s", err->message);
			g_error_free(err);
		}

	}

	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile) &&
			t->sco.rfcomm != NULL) {
		/* notify associated RFCOMM transport */
		ba_transport_send_signal(t->sco.rfcomm, BA_TRANSPORT_SIGNAL_UPDATE_VOLUME);
	}

	/* notify connected clients (including requester) */
	bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_VOLUME);

	return 0;
}

int ba_transport_set_state(struct ba_transport *t, enum ba_transport_state state) {
	debug("State transition: %d -> %d", t->state, state);

	if (t->state == state)
		return 0;

	/* For the A2DP sink profile, the IO thread can not be created until the
	 * BT transport is acquired, otherwise thread initialized will fail. */
	if (t->type.profile == BA_TRANSPORT_PROFILE_A2DP_SINK &&
			t->state == BA_TRANSPORT_STATE_IDLE &&
			state != BA_TRANSPORT_STATE_PENDING)
		return 0;

	int ret = 0;

	t->state = state;

	switch (state) {
	case BA_TRANSPORT_STATE_IDLE:
		ba_transport_pthread_cancel(t);
		break;
	case BA_TRANSPORT_STATE_PENDING:
		/* When transport is marked as pending, try to acquire transport, but only
		 * if we are handing A2DP sink profile. For source profile, transport has
		 * to be acquired by our controller (during the PCM open request). */
		if (t->type.profile == BA_TRANSPORT_PROFILE_A2DP_SINK)
			ret = t->acquire(t);
		break;
	case BA_TRANSPORT_STATE_ACTIVE:
	case BA_TRANSPORT_STATE_PAUSED:
		if (pthread_equal(t->thread, config.main_thread)) {
			if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
				ret = a2dp_thread_create(t);
			else if (t->type.profile & BA_TRANSPORT_PROFILE_RFCOMM)
				ret = ba_transport_pthread_create(t, rfcomm_thread, "ba-rfcomm");
			else
				ret = ba_transport_pthread_create(t, sco_thread, "ba-sco");
		}
		break;
	}

	/* something went wrong, so go back to idle */
	if (ret == -1)
		return ba_transport_set_state(t, BA_TRANSPORT_STATE_IDLE);

	return ret;
}

int ba_transport_drain_pcm(struct ba_transport *t) {

	pthread_mutex_t *mutex = NULL;
	pthread_cond_t *drained = NULL;

	switch (t->type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		mutex = &t->a2dp.drained_mtx;
		drained = &t->a2dp.drained;
		break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
	case BA_TRANSPORT_PROFILE_HSP_AG:
		mutex = &t->sco.spk_drained_mtx;
		drained = &t->sco.spk_drained;
		break;
	}

	if (mutex == NULL || t->state != BA_TRANSPORT_STATE_ACTIVE)
		return 0;

	pthread_mutex_lock(mutex);

	ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PCM_SYNC);
	pthread_cond_wait(drained, mutex);

	pthread_mutex_unlock(mutex);

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

static int transport_acquire_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg, *rep;
	GUnixFDList *fd_list;
	GError *err = NULL;

	/* Check whether transport is already acquired - keep-alive mode. */
	if (t->bt_fd != -1) {
		debug("Reusing transport: %d", t->bt_fd);
		goto final;
	}

	msg = g_dbus_message_new_method_call(t->bluez_dbus_owner,
			t->bluez_dbus_path, BLUEZ_IFACE_MEDIA_TRANSPORT,
			t->state == BA_TRANSPORT_STATE_PENDING ? "TryAcquire" : "Acquire");

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

	/* Minimize audio delay and increase responsiveness (seeking, stopping) by
	 * decreasing the BT socket output buffer. We will use a tripled write MTU
	 * value, in order to prevent tearing due to temporal heavy load. */
	size_t size = t->mtu_write * 3;
	if (setsockopt(t->bt_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1)
		warn("Couldn't set socket output buffer size: %s", strerror(errno));

	if (ioctl(t->bt_fd, TIOCOUTQ, &t->a2dp.bt_fd_coutq_init) == -1)
		warn("Couldn't get socket queued bytes: %s", strerror(errno));

	debug("New transport: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

fail:
	g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't acquire transport: %s", err->message);
		g_error_free(err);
	}

final:
	return t->bt_fd;
}

static int transport_release_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	/* If the transport has not been acquired, or it has been released already,
	 * there is no need to release it again. In fact, trying to release already
	 * closed transport will result in returning error message. */
	if (t->bt_fd == -1)
		return 0;

	debug("Releasing transport: %s", ba_transport_type_to_string(t->type));

	/* If the state is idle, it means that either transport was not acquired, or
	 * was released by the BlueZ. In both cases there is no point in a explicit
	 * release request. It might even return error (e.g. not authorized). */
	if (t->state != BA_TRANSPORT_STATE_IDLE && t->bluez_dbus_owner != NULL) {

		msg = g_dbus_message_new_method_call(t->bluez_dbus_owner, t->bluez_dbus_path,
				BLUEZ_IFACE_MEDIA_TRANSPORT, "Release");

		if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
						G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
			goto fail;

		if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
			g_dbus_message_to_gerror(rep, &err);
			if (err->code == G_DBUS_ERROR_NO_REPLY ||
					err->code == G_DBUS_ERROR_SERVICE_UNKNOWN) {
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

static int transport_release_bt_rfcomm(struct ba_transport *t) {

	if (t->bt_fd == -1)
		return 0;

	debug("Closing RFCOMM: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

	/* BlueZ does not trigger profile disconnection signal when the Bluetooth
	 * link has been lost (e.g. device power down). However, it is required to
	 * remove all references, otherwise resources will not be freed. */
	bluealsa_dbus_rfcomm_unregister(t);

	if (t->rfcomm.sco != NULL) {
		ba_transport_destroy(t->rfcomm.sco);
		t->rfcomm.sco = NULL;
	}

	return 0;
}

static int transport_acquire_bt_sco(struct ba_transport *t) {

	if (t->bt_fd != -1) {
		debug("Reusing SCO: %d", t->bt_fd);
		return t->bt_fd;
	}

	if ((t->bt_fd = hci_sco_open(t->d->a->hci.dev_id)) == -1) {
		error("Couldn't open SCO socket: %s", strerror(errno));
		goto fail;
	}

	if (hci_sco_connect(t->bt_fd, &t->d->addr,
				t->type.codec == HFP_CODEC_CVSD ? BT_VOICE_CVSD_16BIT : BT_VOICE_TRANSPARENT) == -1) {
		error("Couldn't establish SCO link: %s", strerror(errno));
		goto fail;
	}

	debug("New SCO link: %s: %d", batostr_(&t->d->addr), t->bt_fd);

	t->mtu_read = t->mtu_write = hci_sco_get_mtu(t->bt_fd);

	return t->bt_fd;

fail:
	if (t->bt_fd != -1)
		close(t->bt_fd);
	t->bt_fd = -1;
	return -1;
}

static int transport_release_bt_sco(struct ba_transport *t) {

	if (t->bt_fd == -1)
		return 0;

	debug("Closing SCO: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

	return 0;
}

int ba_transport_release_pcm(struct ba_transport_pcm *pcm) {

	if (pcm->fd == -1)
		return 0;

	int oldstate;

	/* Transport IO workers are managed using thread cancellation mechanism,
	 * so we have to take into account a possibility of cancellation during the
	 * execution. In this release function it is important to perform actions
	 * atomically. Since close call is a cancellation point, it is required to
	 * temporally disable cancellation. For a better understanding of what is
	 * going on, see the io_thread_read_pcm() function. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

	debug("Closing PCM: %d", pcm->fd);
	close(pcm->fd);
	pcm->fd = -1;
	pcm->client = -1;

	pthread_setcancelstate(oldstate, NULL);
	return 0;
}

/**
 * Create transport thread. */
int ba_transport_pthread_create(
		struct ba_transport *t,
		void *(*routine)(struct ba_transport *),
		const char *name) {

	int ret;

	if ((ret = pthread_create(&t->thread, NULL,
					PTHREAD_ROUTINE(routine), ba_transport_ref(t))) != 0) {
		error("Couldn't create transport thread: %s", strerror(ret));
		t->thread = config.main_thread;
		ba_transport_unref(t);
		return -1;
	}

	pthread_setname_np(t->thread, name);
	debug("Created new thread [%s]: %s", name, ba_transport_type_to_string(t->type));

	return 0;
}

/**
 * Wrapper for release callback, which can be used by the pthread cleanup.
 *
 * This function CAN be used with ba_transport_pthread_cleanup_lock() in order
 * to guard transport critical section during cleanup process. */
void ba_transport_pthread_cleanup(struct ba_transport *t) {

	/* During the normal operation mode, the release callback should not
	 * be NULL. Hence, we will relay on this callback - file descriptors
	 * are closed in it. */
	if (t->release != NULL)
		t->release(t);

	ba_transport_pthread_cleanup_unlock(t);

	/* XXX: If the order of the cleanup push is right, this function will
	 *      indicate the end of the IO/RFCOMM thread. */
	debug("Exiting IO thread: %s", ba_transport_type_to_string(t->type));

	/* Remove reference which was taken by the io_thread_create(). */
	ba_transport_unref(t);
}

int ba_transport_pthread_cleanup_lock(struct ba_transport *t) {
	int ret = pthread_mutex_lock(&t->mutex);
	t->cleanup_lock = true;
	return ret;
}

int ba_transport_pthread_cleanup_unlock(struct ba_transport *t) {
	if (!t->cleanup_lock)
		return 0;
	t->cleanup_lock = false;
	return pthread_mutex_unlock(&t->mutex);
}
