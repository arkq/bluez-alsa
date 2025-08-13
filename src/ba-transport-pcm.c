/*
 * BlueALSA - ba-transport-pcm.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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

#include <gio/gio.h>
#include <glib.h>

#include "audio.h"
#include "ba-config.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "dbus.h"
#include "hfp.h"
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
		struct ba_transport *t,
		bool master) {

	pcm->t = t;
	pcm->mode = mode;
	pcm->master = master;
	pcm->state = BA_TRANSPORT_PCM_STATE_TERMINATED;
	pcm->fd = -1;
	pcm->fd_bt = -1;
	pcm->pipe[0] = -1;
	pcm->pipe[1] = -1;

	for (size_t i = 0; i < ARRAYSIZE(pcm->volume); i++) {
		pcm->volume[i].level = config.volume_init_level;
		ba_transport_pcm_volume_set(&pcm->volume[i], NULL, NULL, NULL);
	}

	pthread_mutex_init(&pcm->mutex, NULL);
	pthread_mutex_init(&pcm->state_mtx, NULL);
	pthread_mutex_init(&pcm->client_mtx, NULL);
	pthread_cond_init(&pcm->cond, NULL);

	if (pipe(pcm->pipe) == -1)
		return -1;

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
	pthread_mutex_destroy(&pcm->state_mtx);
	pthread_mutex_destroy(&pcm->client_mtx);
	pthread_cond_destroy(&pcm->cond);

	if (pcm->pipe[0] != -1)
		close(pcm->pipe[0]);
	if (pcm->pipe[1] != -1)
		close(pcm->pipe[1]);

	g_free(pcm->ba_dbus_path);

}

/**
 * Set transport PCM state.
 *
 * It is only allowed to set the new state according to the state machine.
 * For details, see comments in this function body.
 *
 * @param pcm Transport PCM.
 * @param state New transport PCM state.
 * @return If state transition was successful, 0 is returned. Otherwise, -1 is
 *   returned and errno is set to EINVAL. */
int ba_transport_pcm_state_set(
		struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_state state) {

	pthread_mutex_lock(&pcm->state_mtx);

	enum ba_transport_pcm_state old_state = pcm->state;

	/* Moving to the next state is always allowed. */
	bool valid = state == pcm->state + 1;

	/* Allow wrapping around the state machine. */
	if (state == BA_TRANSPORT_PCM_STATE_IDLE &&
			old_state == BA_TRANSPORT_PCM_STATE_TERMINATED)
		valid = true;

	/* Thread initialization failure: STARTING -> STOPPING */
	if (state == BA_TRANSPORT_PCM_STATE_STOPPING &&
			old_state == BA_TRANSPORT_PCM_STATE_STARTING)
		valid = true;

	/* Additionally, it is allowed to move to the TERMINATED state from
	 * IDLE and STARTING. This transition indicates that the thread has
	 * never been started or there was an error during the startup. */
	if (state == BA_TRANSPORT_PCM_STATE_TERMINATED && (
				old_state == BA_TRANSPORT_PCM_STATE_IDLE ||
				old_state == BA_TRANSPORT_PCM_STATE_STARTING))
		valid = true;

	if (valid)
		pcm->state = state;

	pthread_mutex_unlock(&pcm->state_mtx);

	if (!valid)
		return errno = EINVAL, -1;

	if (state != old_state && (
				state == BA_TRANSPORT_PCM_STATE_RUNNING ||
				old_state == BA_TRANSPORT_PCM_STATE_RUNNING)) {
			bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_RUNNING);
	}

	pthread_cond_broadcast(&pcm->cond);
	return 0;
}

/**
 * Check if transport PCM is in given state. */
bool ba_transport_pcm_state_check(
		const struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_state state) {
	pthread_mutex_lock(MUTABLE(&pcm->state_mtx));
	bool ok = pcm->state == state;
	pthread_mutex_unlock(MUTABLE(&pcm->state_mtx));
	return ok;
}

/**
 * Wait until transport PCM reaches given state. */
int ba_transport_pcm_state_wait(
		const struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_state state) {

	enum ba_transport_pcm_state tmp;

	pthread_mutex_lock(MUTABLE(&pcm->state_mtx));
	while ((tmp = pcm->state) < state)
		pthread_cond_wait(MUTABLE(&pcm->cond), MUTABLE(&pcm->state_mtx));
	pthread_mutex_unlock(MUTABLE(&pcm->state_mtx));

	if (tmp == state)
		return 0;

	errno = EIO;
	return -1;
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

	/* The thread may have been cancelled while a PCM drain operation
	 * is in progress. To prevent ba_transport_pcm_drain() from blocking
	 * forever, we signal that drain is no longer in progress. */
	pthread_mutex_lock(&pcm->mutex);
	pcm->drained = true;
	pthread_mutex_unlock(&pcm->mutex);
	pthread_cond_signal(&pcm->cond);

	/* For proper functioning of the transport, all threads have to be
	 * operational. Therefore, if one of the threads is being cancelled,
	 * we have to cancel all other threads. */
	pthread_mutex_lock(&t->bt_fd_mtx);
	ba_transport_stop_async(t);
	pthread_mutex_unlock(&t->bt_fd_mtx);

	/* Release BT socket file descriptor duplicate created either in the
	 * ba_transport_pcm_start() function or in the IO thread itself. */
	ba_transport_pcm_bt_release(pcm);

	/* If we are closing master PCM, release underlying BT transport. */
	if (pcm->master)
		ba_transport_release(t);

#if DEBUG
	/* XXX: If the order of the cleanup push is right, this function will
	 *      indicate the end of the transport IO thread. */
	char name[32];
	pthread_getname_np(pcm->tid, name, sizeof(name));
	debug("Exiting IO thread [%s]: %s", name, ba_transport_debug_name(t));
#endif

	/* Remove reference which was taken by the ba_transport_pcm_start(). */
	ba_transport_unref(t);
}

int ba_transport_pcm_bt_acquire(struct ba_transport_pcm *pcm) {

	struct ba_transport *t = pcm->t;
	int ret = -1;

	if (pcm->fd_bt != -1)
		return 0;

	pthread_mutex_lock(&t->bt_fd_mtx);

	const int bt_fd = t->bt_fd;

	/* check if BT socket file descriptor is valid */
	if (bt_fd == -1) {
		error("Invalid BT socket: %d", bt_fd);
		goto fail;
	}

	/* check for invalid (i.e. not set) MTU values */
	if (t->mtu_read == 0 || t->mtu_write == 0) {
		error("Invalid BT socket MTU [%d]: R:%zu W:%zu", bt_fd,
				t->mtu_read, t->mtu_write);
		goto fail;
	}

	if ((pcm->fd_bt = dup(bt_fd)) == -1) {
		error("Couldn't duplicate BT socket [%d]: %s", bt_fd, strerror(errno));
		goto fail;
	}

	debug("Created BT socket duplicate: [%d]: %d", bt_fd, pcm->fd_bt);
	ret = 0;

fail:
	pthread_mutex_unlock(&t->bt_fd_mtx);
	return ret;
}

int ba_transport_pcm_bt_release(struct ba_transport_pcm *pcm) {

	if (pcm->fd_bt != -1) {
#if DEBUG
		pthread_mutex_lock(&pcm->t->bt_fd_mtx);
		debug("Closing BT socket duplicate [%d]: %d", pcm->t->bt_fd, pcm->fd_bt);
		pthread_mutex_unlock(&pcm->t->bt_fd_mtx);
#endif
		close(pcm->fd_bt);
		pcm->fd_bt = -1;
	}

	return 0;
}

/**
 * Start transport PCM thread. */
int ba_transport_pcm_start(
		struct ba_transport_pcm *pcm,
		ba_transport_pcm_thread_func th_func,
		const char *name) {

	struct ba_transport *t = pcm->t;
	sigset_t sigset, oldset;
	int ret = -1;

	pthread_mutex_lock(&pcm->state_mtx);

	pcm->state = BA_TRANSPORT_PCM_STATE_STARTING;

	/* Please note, this call here does not guarantee that the BT socket
	 * will be acquired, because transport might not be opened yet. */
	if (ba_transport_pcm_bt_acquire(pcm) == -1) {
		pcm->state = BA_TRANSPORT_PCM_STATE_TERMINATED;
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

	if ((ret = pthread_create(&pcm->tid, NULL, PTHREAD_FUNC(th_func), pcm)) != 0) {
		error("Couldn't create IO thread: %s", strerror(ret));
		pcm->state = BA_TRANSPORT_PCM_STATE_TERMINATED;
		pthread_sigmask(SIG_SETMASK, &oldset, NULL);
		ba_transport_unref(t);
		goto fail;
	}

	if (config.io_thread_rt_priority != 0) {
		struct sched_param param = { .sched_priority = config.io_thread_rt_priority };
		if ((ret = pthread_setschedparam(pcm->tid, SCHED_FIFO, &param)) != 0)
			warn("Couldn't set IO thread RT priority: %s", strerror(ret));
		/* It's not a fatal error if we can't set thread priority. */
		ret = 0;
	}

	pthread_sigmask(SIG_SETMASK, &oldset, NULL);

	pthread_setname_np(pcm->tid, name);
	debug("Created new IO thread [%s]: %s", name, ba_transport_debug_name(t));

fail:
	pthread_mutex_unlock(&pcm->state_mtx);
	pthread_cond_broadcast(&pcm->cond);
	return ret == 0 ? 0 : -1;
}

/**
 * Stop transport PCM thread in a synchronous manner.
 *
 * Please be aware that when using this function caller shall not hold
 * any mutex which might be used in the IO thread. Mutex locking is not
 * a cancellation point, so the IO thread might get stuck - it will not
 * terminate, so join will not return either! */
void ba_transport_pcm_stop(
		struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->state_mtx);

	/* If the transport thread is in the idle state (i.e. it is not running),
	 * we can mark it as terminated right away. */
	if (pcm->state == BA_TRANSPORT_PCM_STATE_IDLE) {
		pcm->state = BA_TRANSPORT_PCM_STATE_TERMINATED;
		pthread_mutex_unlock(&pcm->state_mtx);
		pthread_cond_broadcast(&pcm->cond);
		return;
	}

	/* If this function was called from more than one thread at the same time
	 * (e.g. from transport thread manager thread and from main thread due to
	 * SIGTERM signal), wait until the IO thread terminates - this function is
	 * supposed to be synchronous. */
	if (pcm->state == BA_TRANSPORT_PCM_STATE_JOINING) {
		while (pcm->state != BA_TRANSPORT_PCM_STATE_TERMINATED)
			pthread_cond_wait(&pcm->cond, &pcm->state_mtx);
		pthread_mutex_unlock(&pcm->state_mtx);
		return;
	}

	if (pcm->state == BA_TRANSPORT_PCM_STATE_TERMINATED) {
		pthread_mutex_unlock(&pcm->state_mtx);
		return;
	}

	/* The transport thread has to be marked for stopping. If at this point
	 * the state is not STOPPING, it is a programming error. */
	g_assert_cmpint(pcm->state, ==, BA_TRANSPORT_PCM_STATE_STOPPING);

	int err;
	pthread_t id = pcm->tid;
	if ((err = pthread_cancel(id)) != 0 && err != ESRCH)
		warn("Couldn't cancel IO thread: %s", strerror(err));

	/* Set the state to JOINING before unlocking the mutex. This will
	 * prevent calling the pthread_cancel() function once again. */
	pcm->state = BA_TRANSPORT_PCM_STATE_JOINING;

	pthread_mutex_unlock(&pcm->state_mtx);

	if ((err = pthread_join(id, NULL)) != 0)
		warn("Couldn't join IO thread: %s", strerror(err));

	pthread_mutex_lock(&pcm->state_mtx);
	pcm->state = BA_TRANSPORT_PCM_STATE_TERMINATED;
	pthread_mutex_unlock(&pcm->state_mtx);

	/* Notify others that the thread has been terminated. */
	pthread_cond_broadcast(&pcm->cond);

}

int ba_transport_pcm_release(struct ba_transport_pcm *pcm) {

#if DEBUG
	/* assert that we were called with the lock held */
	g_assert_cmpint(pthread_mutex_trylock(&pcm->mutex), !=, 0);
#endif

	if (pcm->fd != -1) {
		debug("Closing PCM: %d", pcm->fd);
		close(pcm->fd);
		pcm->fd = -1;
	}

	if (pcm->controller != NULL) {
		g_source_destroy(pcm->controller);
		g_source_unref(pcm->controller);
		pcm->controller = NULL;
	}

	return 0;
}

int ba_transport_pcm_pause(struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);
	debug("PCM pause: %d", pcm->fd);
	pcm->paused = true;
	pthread_mutex_unlock(&pcm->mutex);

	return ba_transport_pcm_signal_send(pcm, BA_TRANSPORT_PCM_SIGNAL_PAUSE);
}

int ba_transport_pcm_resume(struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);
	debug("PCM resume: %d", pcm->fd);
	pcm->paused = false;
	pthread_mutex_unlock(&pcm->mutex);

	return ba_transport_pcm_signal_send(pcm, BA_TRANSPORT_PCM_SIGNAL_RESUME);
}

int ba_transport_pcm_drain(struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);

	if (!ba_transport_pcm_state_check_running(pcm)) {
		pthread_mutex_unlock(&pcm->mutex);
		return errno = ESRCH, -1;
	}

	debug("PCM drain: %d", pcm->fd);

	pcm->drained = false;
	ba_transport_pcm_signal_send(pcm, BA_TRANSPORT_PCM_SIGNAL_DRAIN);

	while (!pcm->drained)
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

	int rv = ba_transport_pcm_signal_send(pcm, BA_TRANSPORT_PCM_SIGNAL_DROP);
	if (rv == -1 && errno == ESRCH) {
		/* If the transport thread is not running flush the PCM here. */
		io_pcm_flush(pcm);
		rv = 0;
	}

	return rv;
}

int ba_transport_pcm_signal_send(
		struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_signal signal) {

	int ret = -1;

	pthread_mutex_lock(&pcm->state_mtx);

	if (pcm->state != BA_TRANSPORT_PCM_STATE_RUNNING) {
		errno = ESRCH;
		goto fail;
	}

	if (write(pcm->pipe[1], &signal, sizeof(signal)) != sizeof(signal)) {
		warn("Couldn't write transport PCM signal: %s", strerror(errno));
		goto fail;
	}

	ret = 0;

fail:
	pthread_mutex_unlock(&pcm->state_mtx);
	return ret;
}

/**
 * Receive signal sent by ba_transport_pcm_signal_send().
 *
 * @note
 * In case of error, this function will return -1 instead of signal value. */
enum ba_transport_pcm_signal ba_transport_pcm_signal_recv(
		struct ba_transport_pcm *pcm) {

	enum ba_transport_pcm_signal signal;
	ssize_t ret;

	while ((ret = read(pcm->pipe[0], &signal, sizeof(signal))) == -1 &&
			errno == EINTR)
		continue;

	if (ret == sizeof(signal))
		return signal;

	warn("Couldn't read transport PCM signal: %s", strerror(errno));
	return -1;
}

bool ba_transport_pcm_is_active(const struct ba_transport_pcm *pcm) {
	pthread_mutex_lock(MUTABLE(&pcm->mutex));
	bool active = pcm->fd != -1 && !pcm->paused;
	pthread_mutex_unlock(MUTABLE(&pcm->mutex));
	return active;
}

/**
 * Convert PCM volume level to [0, max] range. */
unsigned int ba_transport_pcm_volume_level_to_range(int value, int max) {
	int volume = audio_decibel_to_loudness(value / 100.0) * max;
	return MIN(MAX(volume, 0), max);
}

/**
 * Convert [0, max] range to PCM volume level. */
int ba_transport_pcm_volume_range_to_level(int value, int max) {
	int level = audio_loudness_to_decibel(1.0 * value / max) * 100;
	return MIN(MAX(level, -9600), 9600);
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

/**
 * Synchronize PCM volume level.
 *
 * This function notifies remote Bluetooth device and D-Bus clients. */
int ba_transport_pcm_volume_sync(struct ba_transport_pcm *pcm, unsigned int update_mask) {

	struct ba_transport *t = pcm->t;

	/* In case of A2DP Source or HSP/HFP Audio Gateway skip notifying Bluetooth
	 * device if we are using software volume control. This will prevent volume
	 * double scaling - firstly by us and then by Bluetooth headset/speaker. */
	if (pcm->soft_volume && t->profile & (
				BA_TRANSPORT_PROFILE_A2DP_SOURCE | BA_TRANSPORT_PROFILE_MASK_AG))
		goto final;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		/* A2DP specification defines volume property as a single value - volume
		 * for only one channel. For multi-channel audio, we will use calculated
		 * average volume level. */

		int level_sum = 0;
		for (size_t i = 0; i < pcm->channels; i++)
			level_sum += pcm->volume[i].level;

		uint16_t volume = ba_transport_pcm_volume_level_to_range(
				level_sum / (int)pcm->channels, BLUEZ_A2DP_VOLUME_MAX);

		/* skip update if nothing has changed */
		if (volume != t->media.volume) {

			GError *err = NULL;
			t->media.volume = volume;
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
	/* Notify all connected D-Bus clients. */
	bluealsa_dbus_pcm_update(pcm, update_mask);
	return 0;
}

/**
 * Get non-software PCM volume level if available. */
int ba_transport_pcm_get_hardware_volume(
		const struct ba_transport_pcm *pcm) {

	const struct ba_transport *t = pcm->t;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return t->media.volume;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {

		if (t->sco.rfcomm == NULL)
			/* TODO: Cache volume level for oFono-based SCO */
			return HFP_VOLUME_GAIN_MAX;

		if (pcm == &t->sco.pcm_spk)
			return t->sco.rfcomm->gain_spk;
		if (pcm == &t->sco.pcm_mic)
			return t->sco.rfcomm->gain_mic;

	}

	g_assert_not_reached();
	return 0;
}

/**
 * Get PCM playback/capture cumulative delay. */
int ba_transport_pcm_delay_get(const struct ba_transport_pcm *pcm) {

	const struct ba_transport *t = pcm->t;
	int delay = 0;

	delay += pcm->codec_delay_dms;
	delay += pcm->processing_delay_dms;

	/* Add delay reported by BlueZ but only for A2DP Source profile. In case
	 * of A2DP Sink, the BlueZ delay value is in fact our client delay. */
	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		delay += t->media.delay;
	/* HFP/HSP profiles do not provide any delay information. However, we can
	 * assume some arbitrary value here - for now it will be 10 ms. */
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_AG)
		delay += 10;

	return delay;
}

/**
 * Synchronize PCM playback delay.
 *
 * This function notifies remote Bluetooth device and D-Bus clients. */
int ba_transport_pcm_delay_sync(struct ba_transport_pcm *pcm, unsigned int update_mask) {

	struct ba_transport *t = pcm->t;

	/* In case of A2DP Sink, update the delay property of the BlueZ media
	 * transport interface. BlueZ should forward this value to the remote
	 * device, so it can adjust audio/video synchronization. */
	if (t->profile == BA_TRANSPORT_PROFILE_A2DP_SINK) {

		int delay = 0;
		delay += pcm->codec_delay_dms;
		delay += pcm->processing_delay_dms;
		delay += pcm->client_delay_dms;

		if (t->media.delay_reporting &&
					abs(delay - t->media.delay) >= 100 /* 10ms */) {

			GError *err = NULL;
			t->media.delay = delay;
			g_dbus_set_property(config.dbus, t->bluez_dbus_owner, t->bluez_dbus_path,
					BLUEZ_IFACE_MEDIA_TRANSPORT, "Delay", g_variant_new_uint16(delay), &err);

			if (err != NULL) {
				if (err->code == G_DBUS_ERROR_PROPERTY_READ_ONLY)
					/* Even though BlueZ documentation says that the Delay
					 * property is read-write, it might not be true. In case
					 * when the delay write operation fails with "not writable"
					 * error, we should not try to update the delay report
					 * value any more. */
					t->media.delay_reporting = false;
				warn("Couldn't set A2DP transport delay: %s", err->message);
				g_error_free(err);
			}

		}
	}

	if (update_mask & BA_DBUS_PCM_UPDATE_DELAY) {
		/* To avoid creating a flood of D-Bus signals, we only notify clients
		 * when the codec + processing value changes by more than 10ms. */
		int delay = pcm->codec_delay_dms + pcm->processing_delay_dms;
		if (abs(delay - (int)pcm->reported_codec_delay_dms) < 100 /* 10ms */)
			goto final;
		pcm->reported_codec_delay_dms = delay;
	}

	/* Notify all connected D-Bus clients. */
	bluealsa_dbus_pcm_update(pcm, update_mask);

final:
	return 0;
}

const char *ba_transport_pcm_channel_to_string(
		enum ba_transport_pcm_channel channel) {
	switch (channel) {
	case BA_TRANSPORT_PCM_CHANNEL_MONO:
		return "MONO";
	case BA_TRANSPORT_PCM_CHANNEL_FL:
		return "FL";
	case BA_TRANSPORT_PCM_CHANNEL_FR:
		return "FR";
	case BA_TRANSPORT_PCM_CHANNEL_FC:
		return "FC";
	case BA_TRANSPORT_PCM_CHANNEL_RL:
		return "RL";
	case BA_TRANSPORT_PCM_CHANNEL_RR:
		return "RR";
	case BA_TRANSPORT_PCM_CHANNEL_SL:
		return "SL";
	case BA_TRANSPORT_PCM_CHANNEL_SR:
		return "SR";
	case BA_TRANSPORT_PCM_CHANNEL_LFE:
		return "LFE";
	default:
		error("Unsupported channel type: %#x", channel);
		g_assert_not_reached();
		return NULL;
	}
}
