/*
 * BlueALSA - ba-transport-pcm.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ba-transport-pcm.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "audio.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "dbus.h"
#include "io.h"
#if ENABLE_OFONO
# include "ofono.h"
#endif
#include "shared/defs.h"
#include "shared/log.h"

static const char *transport_get_dbus_path_type(
		enum ba_transport_profile profile) {
	switch (profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		return "a2dpsrc";
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		return "a2dpsnk";
	case BA_TRANSPORT_PROFILE_HFP_HF:
		return "hfphf";
	case BA_TRANSPORT_PROFILE_HFP_AG:
		return "hfpag";
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "hsphs";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "hspag";
	default:
		return NULL;
	}
}

int transport_pcm_init(
		struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_mode mode,
		struct ba_transport_thread *th) {

	struct ba_transport *t = th->t;

	pcm->t = t;
	pcm->mode = mode;
	pcm->fd = -1;
	pcm->active = true;

	/* link PCM and transport thread */
	pcm->th = th;
	th->pcm = pcm;

	pcm->volume[0].level = config.volume_init_level;
	pcm->volume[1].level = config.volume_init_level;
	ba_transport_pcm_volume_set(&pcm->volume[0], NULL, NULL, NULL);
	ba_transport_pcm_volume_set(&pcm->volume[1], NULL, NULL, NULL);

	pthread_mutex_init(&pcm->mutex, NULL);
	pthread_mutex_init(&pcm->delay_adjustments_mtx, NULL);
	pthread_mutex_init(&pcm->client_mtx, NULL);
	pthread_cond_init(&pcm->cond, NULL);

	pcm->delay_adjustments = g_hash_table_new(NULL, NULL);

	pcm->ba_dbus_path = g_strdup_printf("%s/%s/%s",
			t->d->ba_dbus_path, transport_get_dbus_path_type(t->profile),
			mode == BA_TRANSPORT_PCM_MODE_SOURCE ? "source" : "sink");

	return 0;
}

void transport_pcm_free(
		struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_release(pcm);
	pthread_mutex_unlock(&pcm->mutex);

	pthread_mutex_destroy(&pcm->mutex);
	pthread_mutex_destroy(&pcm->delay_adjustments_mtx);
	pthread_mutex_destroy(&pcm->client_mtx);
	pthread_cond_destroy(&pcm->cond);

	g_hash_table_unref(pcm->delay_adjustments);
	g_free(pcm->ba_dbus_path);

}

struct ba_transport_pcm *ba_transport_pcm_ref(struct ba_transport_pcm *pcm) {
	ba_transport_ref(pcm->t);
	return pcm;
}

void ba_transport_pcm_unref(struct ba_transport_pcm *pcm) {
	ba_transport_unref(pcm->t);
}

/**
 * Transport IO thread cleanup function for pthread cleanup. */
void ba_transport_pcm_thread_cleanup(struct ba_transport_pcm *pcm) {

	struct ba_transport *t = pcm->t;
	struct ba_transport_thread *th = pcm->th;

	/* For proper functioning of the transport, all threads have to be
	 * operational. Therefore, if one of the threads is being cancelled,
	 * we have to cancel all other threads. */
	pthread_mutex_lock(&t->bt_fd_mtx);
	ba_transport_stop_async(t);
	pthread_mutex_unlock(&t->bt_fd_mtx);

	/* Release BT socket file descriptor duplicate created either in the
	 * ba_transport_pcm_start() function or in the IO thread itself. */
	ba_transport_thread_bt_release(th);

	/* If we are closing master thread, release underlying BT transport. */
	if (th->master)
		ba_transport_release(t);

#if DEBUG
	/* XXX: If the order of the cleanup push is right, this function will
	 *      indicate the end of the transport IO thread. */
	char name[32];
	pthread_getname_np(th->id, name, sizeof(name));
	debug("Exiting IO thread [%s]: %s", name, ba_transport_debug_name(t));
#endif

	/* Remove reference which was taken by the ba_transport_pcm_start(). */
	ba_transport_unref(t);
}

/**
 * Start transport PCM thread. */
int ba_transport_pcm_start(
		struct ba_transport_pcm *pcm,
		ba_transport_pcm_thread_func th_func,
		const char *name,
		bool master) {

	struct ba_transport *t = pcm->t;
	struct ba_transport_thread *th = pcm->th;
	sigset_t sigset, oldset;
	int ret = -1;

	pthread_mutex_lock(&th->mutex);

	th->master = master;
	th->state = BA_TRANSPORT_THREAD_STATE_STARTING;

	/* Please note, this call here does not guarantee that the BT socket
	 * will be acquired, because transport might not be opened yet. */
	if (ba_transport_thread_bt_acquire(th) == -1) {
		th->state = BA_TRANSPORT_THREAD_STATE_TERMINATED;
		goto fail;
	}

	ba_transport_ref(t);

	/* Before creating a new thread, we have to block all signals (new thread
	 * will inherit signal mask). This is required, because we are using thread
	 * cancellation for stopping transport thread, and it seems that the
	 * cancellation can deadlock if some signal handler, which uses POSIX API
	 * which is a cancellation point, is called during the initial phase of the
	 * thread cancellation. On top of that BlueALSA uses g_unix_signal_add()
	 * for handling signals, which internally uses signal handler function which
	 * calls write() for notifying the main loop about the signal. All that can
	 * lead to deadlock during SIGTERM handling. */
	sigfillset(&sigset);
	if ((ret = pthread_sigmask(SIG_SETMASK, &sigset, &oldset)) != 0)
		warn("Couldn't set signal mask: %s", strerror(ret));

	if ((ret = pthread_create(&th->id, NULL, PTHREAD_FUNC(th_func), pcm)) != 0) {
		error("Couldn't create IO thread: %s", strerror(ret));
		th->state = BA_TRANSPORT_THREAD_STATE_TERMINATED;
		pthread_sigmask(SIG_SETMASK, &oldset, NULL);
		ba_transport_unref(t);
		goto fail;
	}

	if (config.io_thread_rt_priority != 0) {
		struct sched_param param = { .sched_priority = config.io_thread_rt_priority };
		if ((ret = pthread_setschedparam(th->id, SCHED_FIFO, &param)) != 0)
			warn("Couldn't set IO thread RT priority: %s", strerror(ret));
		/* It's not a fatal error if we can't set thread priority. */
		ret = 0;
	}

	pthread_sigmask(SIG_SETMASK, &oldset, NULL);

	pthread_setname_np(th->id, name);
	debug("Created new IO thread [%s]: %s", name, ba_transport_debug_name(t));

fail:
	pthread_mutex_unlock(&th->mutex);
	pthread_cond_broadcast(&th->cond);
	return ret == 0 ? 0 : -1;
}

int ba_transport_pcm_release(struct ba_transport_pcm *pcm) {

#if DEBUG
	if (pcm->t->profile != BA_TRANSPORT_PROFILE_NONE)
		/* assert that we were called with the lock held */
		g_assert_cmpint(pthread_mutex_trylock(&pcm->mutex), !=, 0);
#endif

	if (pcm->fd == -1)
		goto final;

	debug("Closing PCM: %d", pcm->fd);
	close(pcm->fd);
	pcm->fd = -1;

final:
	return 0;
}

int ba_transport_pcm_pause(struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);
	debug("PCM pause: %d", pcm->fd);
	pcm->active = false;
	pthread_mutex_unlock(&pcm->mutex);

	return ba_transport_thread_signal_send(pcm->th, BA_TRANSPORT_THREAD_SIGNAL_PCM_PAUSE);
}

int ba_transport_pcm_resume(struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);
	debug("PCM resume: %d", pcm->fd);
	pcm->active = true;
	pthread_mutex_unlock(&pcm->mutex);

	return ba_transport_thread_signal_send(pcm->th, BA_TRANSPORT_THREAD_SIGNAL_PCM_RESUME);
}

int ba_transport_pcm_drain(struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);

	if (!ba_transport_thread_state_check_running(pcm->th)) {
		pthread_mutex_unlock(&pcm->mutex);
		return errno = ESRCH, -1;
	}

	debug("PCM drain: %d", pcm->fd);

	pcm->synced = false;
	ba_transport_thread_signal_send(pcm->th, BA_TRANSPORT_THREAD_SIGNAL_PCM_SYNC);

	while (!pcm->synced)
		pthread_cond_wait(&pcm->cond, &pcm->mutex);

	pthread_mutex_unlock(&pcm->mutex);

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

int ba_transport_pcm_drop(struct ba_transport_pcm *pcm) {

#if DEBUG
	pthread_mutex_lock(&pcm->mutex);
	debug("PCM drop: %d", pcm->fd);
	pthread_mutex_unlock(&pcm->mutex);
#endif

	if (io_pcm_flush(pcm) == -1)
		return -1;

	int rv = ba_transport_thread_signal_send(pcm->th, BA_TRANSPORT_THREAD_SIGNAL_PCM_DROP);
	if (rv == -1 && errno == ESRCH)
		rv = 0;

	return rv;
}

bool ba_transport_pcm_is_active(const struct ba_transport_pcm *pcm) {
	pthread_mutex_lock(MUTABLE(&pcm->mutex));
	bool active = pcm->fd != -1 && pcm->active;
	pthread_mutex_unlock(MUTABLE(&pcm->mutex));
	return active;
}

/**
 * Convert PCM volume level to [0, max] range. */
int ba_transport_pcm_volume_level_to_range(int value, int max) {
	int volume = audio_decibel_to_loudness(value / 100.0) * max;
	return MIN(MAX(volume, 0), max);
}

/**
 * Convert [0, max] range to PCM volume level. */
int ba_transport_pcm_volume_range_to_level(int value, int max) {
	double level = audio_loudness_to_decibel(1.0 * value / max);
	return MIN(MAX(level, -96.0), 96.0) * 100;
}

/**
 * Set PCM volume level/mute.
 *
 * One shall use this function instead of directly writing to PCM volume
 * structure fields.
 *
 * @param level If not NULL, new PCM volume level in "dB * 100".
 * @param soft_mute If not NULL, change software mute state.
 * @param hard_mute If not NULL, change hardware mute state. */
void ba_transport_pcm_volume_set(
		struct ba_transport_pcm_volume *volume,
		const int *level,
		const bool *soft_mute,
		const bool *hard_mute) {

	if (level != NULL)
		volume->level = *level;
	/* Allow software mute state modifications only if hardware mute
	 * was not enabled or we are updating software and hardware mute
	 * at the same time. */
	if (soft_mute != NULL &&
			(!volume->hard_mute || hard_mute != NULL))
		volume->soft_mute = *soft_mute;
	if (hard_mute != NULL)
		volume->hard_mute = *hard_mute;

	/* calculate PCM scale factor */
	const bool muted = volume->soft_mute || volume->hard_mute;
	volume->scale = muted ? 0 : pow(10, (0.01 * volume->level) / 20);

}

int ba_transport_pcm_volume_update(struct ba_transport_pcm *pcm) {

	struct ba_transport *t = pcm->t;

	/* In case of A2DP Source or HSP/HFP Audio Gateway skip notifying Bluetooth
	 * device if we are using software volume control. This will prevent volume
	 * double scaling - firstly by us and then by Bluetooth headset/speaker. */
	if (pcm->soft_volume && t->profile & (
				BA_TRANSPORT_PROFILE_A2DP_SOURCE | BA_TRANSPORT_PROFILE_MASK_AG))
		goto final;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		/* A2DP specification defines volume property as a single value - volume
		 * for only one channel. For stereo audio we will use an average of left
		 * and right PCM channels. */

		unsigned int volume;
		switch (pcm->channels) {
		case 1:
			volume = ba_transport_pcm_volume_level_to_range(
					pcm->volume[0].level, BLUEZ_A2DP_VOLUME_MAX);
			break;
		case 2:
			volume = ba_transport_pcm_volume_level_to_range(
					(pcm->volume[0].level + pcm->volume[1].level) / 2,
					BLUEZ_A2DP_VOLUME_MAX);
			break;
		default:
			g_assert_not_reached();
		}

		/* skip update if nothing has changed */
		if (volume != t->a2dp.volume) {

			GError *err = NULL;
			t->a2dp.volume = volume;
			g_dbus_set_property(config.dbus, t->bluez_dbus_owner, t->bluez_dbus_path,
					BLUEZ_IFACE_MEDIA_TRANSPORT, "Volume", g_variant_new_uint16(volume), &err);

			if (err != NULL) {
				warn("Couldn't set BT device volume: %s", err->message);
				g_error_free(err);
			}

		}

	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {

		if (t->sco.rfcomm != NULL)
			/* notify associated RFCOMM transport */
			ba_rfcomm_send_signal(t->sco.rfcomm, BA_RFCOMM_SIGNAL_UPDATE_VOLUME);
#if ENABLE_OFONO
		else
			ofono_call_volume_update(t);
#endif

	}

final:
	/* notify connected clients (including requester) */
	bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_VOLUME);
	return 0;
}

int ba_transport_pcm_get_delay(const struct ba_transport_pcm *pcm) {

	const struct ba_transport *t = pcm->t;
	int delay = pcm->delay + ba_transport_pcm_delay_adjustment_get(pcm);

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		delay += t->a2dp.delay;
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		delay += 10;

	return delay;
}

int16_t ba_transport_pcm_delay_adjustment_get(
		const struct ba_transport_pcm *pcm) {

	struct ba_transport *t = pcm->t;
	uint16_t codec_id = ba_transport_get_codec(t);
	int16_t adjustment = 0;

	pthread_mutex_lock(MUTABLE(&pcm->delay_adjustments_mtx));
	void *val = g_hash_table_lookup(pcm->delay_adjustments, GINT_TO_POINTER(codec_id));
	pthread_mutex_unlock(MUTABLE(&pcm->delay_adjustments_mtx));

	if (val != NULL)
		adjustment = GPOINTER_TO_INT(val);

	return adjustment;
}

void ba_transport_pcm_delay_adjustment_set(
		struct ba_transport_pcm *pcm,
		uint16_t codec_id,
		int16_t adjustment) {
	pthread_mutex_lock(&pcm->delay_adjustments_mtx);
	g_hash_table_insert(pcm->delay_adjustments,
			GINT_TO_POINTER(codec_id), GINT_TO_POINTER(adjustment));
	pthread_mutex_unlock(&pcm->delay_adjustments_mtx);
}
