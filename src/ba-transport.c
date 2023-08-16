/*
 * BlueALSA - ba-transport.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ba-transport.h"
/* IWYU pragma: no_include "config.h" */

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#if ENABLE_AAC
# include "a2dp-aac.h"
#endif
#if ENABLE_APTX
# include "a2dp-aptx.h"
#endif
#if ENABLE_APTX_HD
# include "a2dp-aptx-hd.h"
#endif
#if ENABLE_FASTSTREAM
# include "a2dp-faststream.h"
#endif
#if ENABLE_LC3PLUS
# include "a2dp-lc3plus.h"
#endif
#if ENABLE_LDAC
# include "a2dp-ldac.h"
#endif
#if ENABLE_MPEG
# include "a2dp-mpeg.h"
#endif
#include "a2dp-sbc.h"
#include "audio.h"
#include "ba-adapter.h"
#include "ba-rfcomm.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "dbus.h"
#include "hci.h"
#include "hfp.h"
#include "io.h"
#if ENABLE_OFONO
# include "ofono.h"
#endif
#include "sco.h"
#include "storage.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"

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

static int transport_pcm_init(
		struct ba_transport_pcm *pcm,
		struct ba_transport_thread *th,
		enum ba_transport_pcm_mode mode) {

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

static void transport_pcm_free(
		struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_release(pcm);
	pthread_mutex_unlock(&pcm->mutex);

	pthread_mutex_destroy(&pcm->mutex);
	pthread_mutex_destroy(&pcm->delay_adjustments_mtx);
	pthread_mutex_destroy(&pcm->client_mtx);
	pthread_cond_destroy(&pcm->cond);

	g_hash_table_unref(pcm->delay_adjustments);

	if (pcm->ba_dbus_path != NULL)
		g_free(pcm->ba_dbus_path);

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

static int ba_transport_pcms_full_lock(struct ba_transport *t) {
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		/* lock client mutexes first to avoid deadlock */
		pthread_mutex_lock(&t->a2dp.pcm.client_mtx);
		pthread_mutex_lock(&t->a2dp.pcm_bc.client_mtx);
		/* lock PCM data mutexes */
		pthread_mutex_lock(&t->a2dp.pcm.mutex);
		pthread_mutex_lock(&t->a2dp.pcm_bc.mutex);
		return 0;
	}
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		/* lock client mutexes first to avoid deadlock */
		pthread_mutex_lock(&t->sco.spk_pcm.client_mtx);
		pthread_mutex_lock(&t->sco.mic_pcm.client_mtx);
		/* lock PCM data mutexes */
		pthread_mutex_lock(&t->sco.spk_pcm.mutex);
		pthread_mutex_lock(&t->sco.mic_pcm.mutex);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

static int ba_transport_pcms_full_unlock(struct ba_transport *t) {
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		pthread_mutex_unlock(&t->a2dp.pcm.mutex);
		pthread_mutex_unlock(&t->a2dp.pcm_bc.mutex);
		pthread_mutex_unlock(&t->a2dp.pcm.client_mtx);
		pthread_mutex_unlock(&t->a2dp.pcm_bc.client_mtx);
		return 0;
	}
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		pthread_mutex_unlock(&t->sco.spk_pcm.mutex);
		pthread_mutex_unlock(&t->sco.mic_pcm.mutex);
		pthread_mutex_unlock(&t->sco.spk_pcm.client_mtx);
		pthread_mutex_unlock(&t->sco.mic_pcm.client_mtx);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

static int transport_thread_init(
		struct ba_transport_thread *th,
		struct ba_transport *t) {

	th->t = t;
	th->state = BA_TRANSPORT_THREAD_STATE_TERMINATED;
	th->bt_fd = -1;
	th->pipe[0] = -1;
	th->pipe[1] = -1;

	pthread_mutex_init(&th->mutex, NULL);
	pthread_cond_init(&th->cond, NULL);

	if (pipe(th->pipe) == -1)
		return -1;

	return 0;
}

/**
 * Synchronous transport thread cancellation.
 *
 * Please be aware that when using this function caller shall not hold
 * any mutex which might be used in the IO thread. Mutex locking is not
 * a cancellation point, so the IO thread might get stuck - it will not
 * terminate, so join will not return either! */
static void transport_thread_cancel(struct ba_transport_thread *th) {

	pthread_mutex_lock(&th->mutex);

	/* If the transport thread is in the idle state (i.e. it is not running),
	 * we can mark it as terminated right away. */
	if (th->state == BA_TRANSPORT_THREAD_STATE_IDLE) {
		th->state = BA_TRANSPORT_THREAD_STATE_TERMINATED;
		pthread_mutex_unlock(&th->mutex);
		pthread_cond_broadcast(&th->cond);
		return;
	}

	/* If this function was called from more than one thread at the same time
	 * (e.g. from transport thread manager thread and from main thread due to
	 * SIGTERM signal), wait until the IO thread terminates - this function is
	 * supposed to be synchronous. */
	if (th->state == BA_TRANSPORT_THREAD_STATE_JOINING) {
		while (th->state != BA_TRANSPORT_THREAD_STATE_TERMINATED)
			pthread_cond_wait(&th->cond, &th->mutex);
		pthread_mutex_unlock(&th->mutex);
		return;
	}

	if (th->state == BA_TRANSPORT_THREAD_STATE_TERMINATED) {
		pthread_mutex_unlock(&th->mutex);
		return;
	}

	/* The transport thread has to be marked for stopping. If at this point
	 * the state is not STOPPING, it is a programming error. */
	g_assert_cmpint(th->state, ==, BA_TRANSPORT_THREAD_STATE_STOPPING);

	int err;
	pthread_t id = th->id;
	if ((err = pthread_cancel(id)) != 0 && err != ESRCH)
		warn("Couldn't cancel transport thread: %s", strerror(err));

	/* Set the state to JOINING before unlocking the mutex. This will
	 * prevent calling the pthread_cancel() function once again. */
	th->state = BA_TRANSPORT_THREAD_STATE_JOINING;

	pthread_mutex_unlock(&th->mutex);

	if ((err = pthread_join(id, NULL)) != 0)
		warn("Couldn't join transport thread: %s", strerror(err));

	pthread_mutex_lock(&th->mutex);
	th->state = BA_TRANSPORT_THREAD_STATE_TERMINATED;
	pthread_mutex_unlock(&th->mutex);

	/* Notify others that the thread has been terminated. */
	pthread_cond_broadcast(&th->cond);

}

/**
 * Release transport thread resources. */
static void transport_thread_free(
		struct ba_transport_thread *th) {
	if (th->bt_fd != -1)
		close(th->bt_fd);
	if (th->pipe[0] != -1)
		close(th->pipe[0]);
	if (th->pipe[1] != -1)
		close(th->pipe[1]);
	pthread_mutex_destroy(&th->mutex);
	pthread_cond_destroy(&th->cond);
}

/**
 * Set transport thread state.
 *
 * It is only allowed to set the new state according to the state machine.
 * For details, see comments in this function body.
 *
 * @param th Transport thread.
 * @param state New transport thread state.
 * @return If state transition was successful, 0 is returned. Otherwise, -1 is
 *   returned and errno is set to EINVAL. */
int ba_transport_thread_state_set(
		struct ba_transport_thread *th,
		enum ba_transport_thread_state state) {

	pthread_mutex_lock(&th->mutex);

	enum ba_transport_thread_state old_state = th->state;

	/* Moving to the next state is always allowed. */
	bool valid = state == th->state + 1;

	/* Allow wrapping around the state machine. */
	if (state == BA_TRANSPORT_THREAD_STATE_IDLE &&
			old_state == BA_TRANSPORT_THREAD_STATE_TERMINATED)
		valid = true;

	/* Thread initialization failure: STARTING -> STOPPING */
	if (state == BA_TRANSPORT_THREAD_STATE_STOPPING &&
			old_state == BA_TRANSPORT_THREAD_STATE_STARTING)
		valid = true;

	/* Additionally, it is allowed to move to the TERMINATED state from
	 * IDLE and STARTING. This transition indicates that the thread has
	 * never been started or there was an error during the startup. */
	if (state == BA_TRANSPORT_THREAD_STATE_TERMINATED && (
				old_state == BA_TRANSPORT_THREAD_STATE_IDLE ||
				old_state == BA_TRANSPORT_THREAD_STATE_STARTING))
		valid = true;

	if (valid)
		th->state = state;

	pthread_mutex_unlock(&th->mutex);

	if (!valid)
		return errno = EINVAL, -1;

	if (state != old_state && (
				state == BA_TRANSPORT_THREAD_STATE_RUNNING ||
				old_state == BA_TRANSPORT_THREAD_STATE_RUNNING)) {
			bluealsa_dbus_pcm_update(th->pcm, BA_DBUS_PCM_UPDATE_RUNNING);
	}

	pthread_cond_broadcast(&th->cond);
	return 0;
}

/**
 * Check if transport thread is in given state. */
bool ba_transport_thread_state_check(
		const struct ba_transport_thread *th,
		enum ba_transport_thread_state state) {
	pthread_mutex_lock(MUTABLE(&th->mutex));
	bool ok = th->state == state;
	pthread_mutex_unlock(MUTABLE(&th->mutex));
	return ok;
}

/**
 * Wait until transport thread reaches given state. */
int ba_transport_thread_state_wait(
		const struct ba_transport_thread *th,
		enum ba_transport_thread_state state) {

	enum ba_transport_thread_state tmp;

	pthread_mutex_lock(MUTABLE(&th->mutex));
	while ((tmp = th->state) < state)
		pthread_cond_wait(MUTABLE(&th->cond), MUTABLE(&th->mutex));
	pthread_mutex_unlock(MUTABLE(&th->mutex));

	if (tmp == state)
		return 0;

	errno = EIO;
	return -1;
}

int ba_transport_thread_bt_acquire(
		struct ba_transport_thread *th) {

	struct ba_transport *t = th->t;
	int ret = -1;

	if (th->bt_fd != -1)
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

	if ((th->bt_fd = dup(bt_fd)) == -1) {
		error("Couldn't duplicate BT socket [%d]: %s", bt_fd, strerror(errno));
		goto fail;
	}

	debug("Created BT socket duplicate: [%d]: %d", bt_fd, th->bt_fd);
	ret = 0;

fail:
	pthread_mutex_unlock(&t->bt_fd_mtx);
	return ret;
}

int ba_transport_thread_bt_release(
		struct ba_transport_thread *th) {

	if (th->bt_fd != -1) {
#if DEBUG
		pthread_mutex_lock(&th->t->bt_fd_mtx);
		debug("Closing BT socket duplicate [%d]: %d", th->t->bt_fd, th->bt_fd);
		pthread_mutex_unlock(&th->t->bt_fd_mtx);
#endif
		close(th->bt_fd);
		th->bt_fd = -1;
	}

	return 0;
}

int ba_transport_thread_signal_send(
		struct ba_transport_thread *th,
		enum ba_transport_thread_signal signal) {

	int ret = -1;

	pthread_mutex_lock(&th->mutex);

	if (th->state != BA_TRANSPORT_THREAD_STATE_RUNNING) {
		errno = ESRCH;
		goto fail;
	}

	if (write(th->pipe[1], &signal, sizeof(signal)) != sizeof(signal)) {
		warn("Couldn't write transport thread signal: %s", strerror(errno));
		goto fail;
	}

	ret = 0;

fail:
	pthread_mutex_unlock(&th->mutex);
	return ret;
}

int ba_transport_thread_signal_recv(
		struct ba_transport_thread *th,
		enum ba_transport_thread_signal *signal) {

	ssize_t ret;
	while ((ret = read(th->pipe[0], signal, sizeof(*signal))) == -1 &&
			errno == EINTR)
		continue;

	if (ret == sizeof(*signal))
		return 0;

	warn("Couldn't read transport thread signal: %s", strerror(errno));
	*signal = BA_TRANSPORT_THREAD_SIGNAL_PING;
	return -1;
}

static void transport_threads_cancel(struct ba_transport *t) {

	transport_thread_cancel(&t->thread_enc);
	transport_thread_cancel(&t->thread_dec);

	pthread_mutex_lock(&t->bt_fd_mtx);
	t->stopping = false;
	pthread_mutex_unlock(&t->bt_fd_mtx);

	pthread_cond_broadcast(&t->stopped);

}

static void transport_threads_cancel_if_no_clients(struct ba_transport *t) {

	/* Hold PCM client and data locks. The data lock is required because we
	 * are going to check the PCM FIFO file descriptor. The client lock is
	 * required to prevent PCM clients from opening PCM in the middle of our
	 * inactivity check. */
	ba_transport_pcms_full_lock(t);

	/* Hold BT lock, because we are going to modify
	 * the IO transports stopping flag. */
	pthread_mutex_lock(&t->bt_fd_mtx);

	bool stop = false;

	if (!t->stopping) {
		if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE) {
			/* Release bidirectional A2DP transport only in case when there
			 * is no active PCM connection - neither encoder nor decoder. */
			if (t->a2dp.pcm.fd == -1 && t->a2dp.pcm_bc.fd == -1)
				t->stopping = stop = true;
		}
		else if (t->profile & BA_TRANSPORT_PROFILE_MASK_AG) {
			/* For Audio Gateway profile it is required to release SCO if we
			 * are not transferring audio (not sending nor receiving), because
			 * it will free Bluetooth bandwidth - headset will send microphone
			 * signal even though we are not reading it! */
			if (t->sco.spk_pcm.fd == -1 && t->sco.mic_pcm.fd == -1)
				t->stopping = stop = true;
		}
	}

	pthread_mutex_unlock(&t->bt_fd_mtx);

	if (stop) {
		debug("Stopping transport: %s", "No PCM clients");
		ba_transport_thread_state_set_stopping(&t->thread_enc);
		ba_transport_thread_state_set_stopping(&t->thread_dec);
	}

	ba_transport_pcms_full_unlock(t);

	if (stop) {
		transport_threads_cancel(t);
	}

}

/**
 * Transport thread manager.
 *
 * This manager handles transport IO threads asynchronous cancellation. */
static void *transport_thread_manager(struct ba_transport *t) {

	pthread_setname_np(pthread_self(), "ba-th-manager");

	struct pollfd fds[] = {
		{ t->thread_manager_pipe[0], POLLIN, 0 }};
	int timeout = -1;

	for (;;) {

		if (poll(fds, ARRAYSIZE(fds), timeout) == 0) {
			transport_threads_cancel_if_no_clients(t);
			timeout = -1;
		}

		if (fds[0].revents & POLLIN) {
			/* incoming manager command */

			enum ba_transport_thread_manager_command cmd;
			if (read(fds[0].fd, &cmd, sizeof(cmd)) != sizeof(cmd)) {
				error("Couldn't read manager command: %s", strerror(errno));
				continue;
			}

			switch (cmd) {
			case BA_TRANSPORT_THREAD_MANAGER_TERMINATE:
				goto exit;
			case BA_TRANSPORT_THREAD_MANAGER_CANCEL_THREADS:
				transport_threads_cancel(t);
				timeout = -1;
				break;
			case BA_TRANSPORT_THREAD_MANAGER_CANCEL_IF_NO_CLIENTS:
				debug("PCM clients check keep-alive: %d ms", config.keep_alive_time);
				timeout = config.keep_alive_time;
				break;
			}

		}

	}

exit:
	return NULL;
}

static int transport_thread_manager_send_command(struct ba_transport *t,
		enum ba_transport_thread_manager_command cmd) {
	if (write(t->thread_manager_pipe[1], &cmd, sizeof(cmd)) == sizeof(cmd))
		return 0;
	error("Couldn't send thread manager command: %s", strerror(errno));
	return -1;
}

/**
 * Create new transport.
 *
 * @param device Pointer to the device structure.
 * @param dbus_owner D-Bus service, which owns this transport.
 * @param dbus_path D-Bus service path for this transport.
 * @return On success, the pointer to the newly allocated transport structure
 *   is returned. If error occurs, NULL is returned and the errno variable is
 *   set to indicated the cause of the error. */
static struct ba_transport *transport_new(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t;
	int err;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		return NULL;

	t->d = ba_device_ref(device);
	t->profile = BA_TRANSPORT_PROFILE_NONE;
	t->codec_id = -1;
	t->ref_count = 1;

	pthread_mutex_init(&t->codec_id_mtx, NULL);
	pthread_mutex_init(&t->codec_select_client_mtx, NULL);
	pthread_mutex_init(&t->bt_fd_mtx, NULL);
	pthread_mutex_init(&t->acquisition_mtx, NULL);
	pthread_cond_init(&t->stopped, NULL);

	t->bt_fd = -1;

	t->thread_manager_thread_id = config.main_thread;
	t->thread_manager_pipe[0] = -1;
	t->thread_manager_pipe[1] = -1;

	err = 0;
	err |= transport_thread_init(&t->thread_enc, t);
	err |= transport_thread_init(&t->thread_dec, t);
	if (err != 0)
		goto fail;

	if (pipe(t->thread_manager_pipe) == -1)
		goto fail;
	if ((errno = pthread_create(&t->thread_manager_thread_id,
			NULL, PTHREAD_FUNC(transport_thread_manager), t)) != 0) {
		t->thread_manager_thread_id = config.main_thread;
		goto fail;
	}

	if ((t->bluez_dbus_owner = strdup(dbus_owner)) == NULL)
		goto fail;
	if ((t->bluez_dbus_path = strdup(dbus_path)) == NULL)
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

static int transport_acquire_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg, *rep;
	GError *err = NULL;
	int fd = -1;

	msg = g_dbus_message_new_method_call(t->bluez_dbus_owner,
			t->bluez_dbus_path, BLUEZ_IFACE_MEDIA_TRANSPORT,
			t->a2dp.state == BLUEZ_A2DP_TRANSPORT_STATE_PENDING ? "TryAcquire" : "Acquire");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	uint16_t mtu_read, mtu_write;
	g_variant_get(g_dbus_message_get_body(rep), "(hqq)",
			NULL, &mtu_read, &mtu_write);

	GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(rep);
	if ((fd = g_unix_fd_list_get(fd_list, 0, &err)) == -1)
		goto fail;

	t->bt_fd = fd;
	t->mtu_read = mtu_read;
	t->mtu_write = mtu_write;

	/* Minimize audio delay and increase responsiveness (seeking, stopping) by
	 * decreasing the BT socket output buffer. We will use a tripled write MTU
	 * value, in order to prevent tearing due to temporal heavy load. */
	size_t size = t->mtu_write * 3;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1)
		warn("Couldn't set socket output buffer size: %s", strerror(errno));

	if (ioctl(fd, TIOCOUTQ, &t->a2dp.bt_fd_coutq_init) == -1)
		warn("Couldn't get socket queued bytes: %s", strerror(errno));

	debug("New A2DP transport: %d", fd);
	debug("A2DP socket MTU: %d: R:%u W:%u", fd, mtu_read, mtu_write);

fail:
	g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't acquire transport: %s", err->message);
		g_error_free(err);
	}

	return fd;
}

static int transport_release_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	/* If the state is idle, it means that either transport was not acquired, or
	 * was released by the BlueZ. In both cases there is no point in a explicit
	 * release request. It might even return error (e.g. not authorized). */
	if (t->a2dp.state != BLUEZ_A2DP_TRANSPORT_STATE_IDLE &&
			t->bluez_dbus_owner != NULL) {

		debug("Releasing A2DP transport: %d", t->bt_fd);

		msg = g_dbus_message_new_method_call(t->bluez_dbus_owner, t->bluez_dbus_path,
				BLUEZ_IFACE_MEDIA_TRANSPORT, "Release");

		if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
						G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
			goto fail;

		if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
			g_dbus_message_to_gerror(rep, &err);
			if (err->code == G_DBUS_ERROR_NO_REPLY ||
					err->code == G_DBUS_ERROR_SERVICE_UNKNOWN ||
					err->code == G_DBUS_ERROR_UNKNOWN_OBJECT) {
				/* If BlueZ is already terminated (or is terminating) or BlueZ
				 * transport interface was already removed (ClearConfiguration
				 * call), we won't receive success response. Do not treat such
				 * a case as an error - omit logging. */
				g_error_free(err);
				err = NULL;
			}
			else
				goto fail;
		}

	}

	debug("Closing A2DP transport: %d", t->bt_fd);

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

struct ba_transport *ba_transport_new_a2dp(
		struct ba_device *device,
		enum ba_transport_profile profile,
		const char *dbus_owner,
		const char *dbus_path,
		const struct a2dp_codec *codec,
		const void *configuration) {

	const bool is_sink = profile & BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t;

	if ((t = transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->profile = profile;

	t->a2dp.codec = codec;
	memcpy(&t->a2dp.configuration, configuration, codec->capabilities_size);
	t->a2dp.state = BLUEZ_A2DP_TRANSPORT_STATE_IDLE;

	transport_pcm_init(&t->a2dp.pcm,
			is_sink ? &t->thread_dec : &t->thread_enc,
			is_sink ? BA_TRANSPORT_PCM_MODE_SOURCE : BA_TRANSPORT_PCM_MODE_SINK);
	t->a2dp.pcm.soft_volume = !config.a2dp.volume;

	transport_pcm_init(&t->a2dp.pcm_bc,
			is_sink ? &t->thread_enc : &t->thread_dec,
			is_sink ?  BA_TRANSPORT_PCM_MODE_SINK : BA_TRANSPORT_PCM_MODE_SOURCE);
	t->a2dp.pcm_bc.soft_volume = !config.a2dp.volume;

	t->acquire = transport_acquire_bt_a2dp;
	t->release = transport_release_bt_a2dp;

	ba_transport_set_codec(t, codec->codec_id);

	storage_pcm_data_sync(&t->a2dp.pcm);
	storage_pcm_data_sync(&t->a2dp.pcm_bc);

	if (t->a2dp.pcm.channels > 0)
		bluealsa_dbus_pcm_register(&t->a2dp.pcm);
	if (t->a2dp.pcm_bc.channels > 0)
		bluealsa_dbus_pcm_register(&t->a2dp.pcm_bc);

	return t;
}

static int transport_acquire_bt_sco(struct ba_transport *t) {

	struct ba_device *d = t->d;
	int fd = -1;

	if ((fd = hci_sco_open(d->a->hci.dev_id)) == -1) {
		error("Couldn't open SCO socket: %s", strerror(errno));
		goto fail;
	}

	struct timespec now;
	struct timespec delay = {
		.tv_nsec = HCI_SCO_CLOSE_CONNECT_QUIRK_DELAY * 1000000 };

	gettimestamp(&now);
	timespecadd(&t->sco.closed_at, &delay, &delay);
	if (difftimespec(&now, &delay, &delay) > 0) {
		info("SCO link close-connect quirk delay: %d ms",
				(int)(delay.tv_nsec / 1000000));
		nanosleep(&delay, NULL);
	}

	const uint16_t codec_id = ba_transport_get_codec(t);
	if (hci_sco_connect(fd, &d->addr,
				codec_id == HFP_CODEC_CVSD ? BT_VOICE_CVSD_16BIT : BT_VOICE_TRANSPARENT) == -1) {
		error("Couldn't establish SCO link: %s", strerror(errno));
		goto fail;
	}

	debug("New SCO link: %s: %d", batostr_(&d->addr), fd);

	t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd, d->a);
	t->bt_fd = fd;

	return fd;

fail:
	if (fd != -1)
		close(fd);
	return -1;
}

static int transport_release_bt_sco(struct ba_transport *t) {

	debug("Releasing SCO link: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

	/* Keep the time-stamp when the SCO link has been closed. It will be used
	 * for calculating close-connect quirk delay in the acquire function. */
	gettimestamp(&t->sco.closed_at);

	return 0;
}

struct ba_transport *ba_transport_new_sco(
		struct ba_device *device,
		enum ba_transport_profile profile,
		const char *dbus_owner,
		const char *dbus_path,
		int rfcomm_fd) {

	const bool is_ag = profile & BA_TRANSPORT_PROFILE_MASK_AG;
	uint16_t codec_id = HFP_CODEC_UNDEFINED;
	struct ba_transport *t;
	int err;

	/* BlueALSA can only support one SCO transport per device, so we arbitrarily
	 * accept only the first profile connection, with no preference for HFP.
	 * Most (all?) commercial devices prefer HFP over HSP, but we are unable to
	 * emulate that with our current design (we would need to know all profiles
	 * supported by the remote device before it connects). Fortunately BlueZ
	 * appears to always connect HFP before HSP, so at least for connections
	 * from commercial devices and for BlueALSA to BlueALSA connections we get
	 * the desired result. */
	if ((t = ba_transport_lookup(device, dbus_path)) != NULL) {
		debug("SCO transport already connected: %s", ba_transport_debug_name(t));
		ba_transport_unref(t);
		errno = EBUSY;
		return NULL;
	}

	if ((t = transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->profile = profile;

	transport_pcm_init(&t->sco.spk_pcm,
			is_ag ? &t->thread_enc : &t->thread_dec,
			is_ag ? BA_TRANSPORT_PCM_MODE_SINK : BA_TRANSPORT_PCM_MODE_SOURCE);
	t->sco.spk_pcm.soft_volume = !config.hfp.volume;

	transport_pcm_init(&t->sco.mic_pcm,
			is_ag ? &t->thread_dec : &t->thread_enc,
			is_ag ? BA_TRANSPORT_PCM_MODE_SOURCE : BA_TRANSPORT_PCM_MODE_SINK);
	t->sco.mic_pcm.soft_volume = !config.hfp.volume;

	t->acquire = transport_acquire_bt_sco;
	t->release = transport_release_bt_sco;

	/* HSP supports CVSD only */
	if (profile & BA_TRANSPORT_PROFILE_MASK_HSP)
		codec_id = HFP_CODEC_CVSD;

#if ENABLE_MSBC
	if (!config.hfp.codecs.msbc)
		codec_id = HFP_CODEC_CVSD;
	/* Check whether support for codec other than
	 * CVSD is possible with underlying adapter. */
	if (!BA_TEST_ESCO_SUPPORT(device->a))
		codec_id = HFP_CODEC_CVSD;
#else
	codec_id = HFP_CODEC_CVSD;
#endif

	ba_transport_set_codec(t, codec_id);

	storage_pcm_data_sync(&t->sco.spk_pcm);
	storage_pcm_data_sync(&t->sco.mic_pcm);

	bluealsa_dbus_pcm_register(&t->sco.spk_pcm);
	bluealsa_dbus_pcm_register(&t->sco.mic_pcm);

	if (rfcomm_fd != -1) {
		if ((t->sco.rfcomm = ba_rfcomm_new(t, rfcomm_fd)) == NULL)
			goto fail;
	}

	return t;

fail:
	err = errno;
	bluealsa_dbus_pcm_unregister(&t->sco.spk_pcm);
	bluealsa_dbus_pcm_unregister(&t->sco.mic_pcm);
	ba_transport_unref(t);
	errno = err;
	return NULL;
}

#if DEBUG
/**
 * Get BlueALSA transport type debug name.
 *
 * @param t Transport structure.
 * @return Human-readable string. */
const char *ba_transport_debug_name(
		const struct ba_transport *t) {
	const enum ba_transport_profile profile = t->profile;
	const uint16_t codec_id = ba_transport_get_codec(t);
	switch (profile) {
	case BA_TRANSPORT_PROFILE_NONE:
		return "NONE";
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (codec_id) {
		case A2DP_CODEC_SBC:
			return "A2DP Source (SBC)";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "A2DP Source (MP3)";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "A2DP Source (AAC)";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "A2DP Source (aptX)";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "A2DP Source (aptX HD)";
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return "A2DP Source (FastStream)";
#endif
#if ENABLE_LC3PLUS
		case A2DP_CODEC_VENDOR_LC3PLUS:
			return "A2DP Source (LC3plus)";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "A2DP Source (LDAC)";
#endif
		} break;
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		switch (codec_id) {
		case A2DP_CODEC_SBC:
			return "A2DP Sink (SBC)";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "A2DP Sink (MP3)";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "A2DP Sink (AAC)";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "A2DP Sink (aptX)";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "A2DP Sink (aptX HD)";
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return "A2DP Sink (FastStream)";
#endif
#if ENABLE_LC3PLUS
		case A2DP_CODEC_VENDOR_LC3PLUS:
			return "A2DP Sink (LC3plus)";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "A2DP Sink (LDAC)";
#endif
		} break;
	case BA_TRANSPORT_PROFILE_HFP_HF:
		switch (codec_id) {
		case HFP_CODEC_UNDEFINED:
			return "HFP Hands-Free (...)";
		case HFP_CODEC_CVSD:
			return "HFP Hands-Free (CVSD)";
		case HFP_CODEC_MSBC:
			return "HFP Hands-Free (mSBC)";
		} break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
		switch (codec_id) {
		case HFP_CODEC_UNDEFINED:
			return "HFP Audio Gateway (...)";
		case HFP_CODEC_CVSD:
			return "HFP Audio Gateway (CVSD)";
		case HFP_CODEC_MSBC:
			return "HFP Audio Gateway (mSBC)";
		} break;
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "HSP Headset";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "HSP Audio Gateway";
	}
	debug("Unknown transport: profile:%#x codec:%#x", profile, codec_id);
	return "N/A";
}
#endif

struct ba_transport *ba_transport_lookup(
		const struct ba_device *device,
		const char *dbus_path) {

	struct ba_transport *t;

	pthread_mutex_lock(MUTABLE(&device->transports_mutex));
	if ((t = g_hash_table_lookup(device->transports, dbus_path)) != NULL)
		t->ref_count++;
	pthread_mutex_unlock(MUTABLE(&device->transports_mutex));

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
 * Unregister D-Bus interfaces, stop IO threads and release transport. */
void ba_transport_destroy(struct ba_transport *t) {

	/* Remove D-Bus interfaces, so no one will access
	 * this transport during the destroy procedure. */
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		bluealsa_dbus_pcm_unregister(&t->a2dp.pcm);
		bluealsa_dbus_pcm_unregister(&t->a2dp.pcm_bc);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		bluealsa_dbus_pcm_unregister(&t->sco.spk_pcm);
		bluealsa_dbus_pcm_unregister(&t->sco.mic_pcm);
		if (t->sco.rfcomm != NULL)
			ba_rfcomm_destroy(t->sco.rfcomm);
		t->sco.rfcomm = NULL;
	}

	/* stop transport IO threads */
	ba_transport_stop(t);

	ba_transport_pcms_full_lock(t);

	/* terminate on-going PCM connections - exit PCM controllers */
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		ba_transport_pcm_release(&t->a2dp.pcm);
		ba_transport_pcm_release(&t->a2dp.pcm_bc);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		ba_transport_pcm_release(&t->sco.spk_pcm);
		ba_transport_pcm_release(&t->sco.mic_pcm);
	}

	/* make sure that transport is released */
	ba_transport_release(t);

	ba_transport_pcms_full_unlock(t);

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

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		storage_pcm_data_update(&t->a2dp.pcm);
		storage_pcm_data_update(&t->a2dp.pcm_bc);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		storage_pcm_data_update(&t->sco.spk_pcm);
		storage_pcm_data_update(&t->sco.mic_pcm);
	}

	debug("Freeing transport: %s", ba_transport_debug_name(t));
	g_assert_cmpint(ref_count, ==, 0);

	if (t->bt_fd != -1)
		close(t->bt_fd);

	ba_device_unref(d);

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		transport_pcm_free(&t->a2dp.pcm);
		transport_pcm_free(&t->a2dp.pcm_bc);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		if (t->sco.rfcomm != NULL)
			ba_rfcomm_destroy(t->sco.rfcomm);
		transport_pcm_free(&t->sco.spk_pcm);
		transport_pcm_free(&t->sco.mic_pcm);
#if ENABLE_OFONO
		free(t->sco.ofono_dbus_path_card);
		free(t->sco.ofono_dbus_path_modem);
#endif
	}

#if DEBUG
	/* If IO threads are not terminated yet, we can not go any further.
	 * Such situation may occur when the transport is about to be freed from one
	 * of the transport IO threads. The transport thread cleanup function sends
	 * a command to the manager to terminate all other threads. In such case, we
	 * will stuck here, because we are about to wait for the transport thread
	 * manager to terminate. But the manager will not terminate, because it is
	 * waiting for a transport thread to terminate - which is us... */
	pthread_mutex_lock(&t->thread_enc.mutex);
	g_assert_cmpint(t->thread_enc.state, ==, BA_TRANSPORT_THREAD_STATE_TERMINATED);
	pthread_mutex_unlock(&t->thread_enc.mutex);
	pthread_mutex_lock(&t->thread_dec.mutex);
	g_assert_cmpint(t->thread_dec.state, ==, BA_TRANSPORT_THREAD_STATE_TERMINATED);
	pthread_mutex_unlock(&t->thread_dec.mutex);
#endif

	if (!pthread_equal(t->thread_manager_thread_id, config.main_thread)) {
		transport_thread_manager_send_command(t, BA_TRANSPORT_THREAD_MANAGER_TERMINATE);
		pthread_join(t->thread_manager_thread_id, NULL);
	}

	transport_thread_free(&t->thread_enc);
	transport_thread_free(&t->thread_dec);

	if (t->thread_manager_pipe[0] != -1)
		close(t->thread_manager_pipe[0]);
	if (t->thread_manager_pipe[1] != -1)
		close(t->thread_manager_pipe[1]);

	pthread_cond_destroy(&t->stopped);
	pthread_mutex_destroy(&t->bt_fd_mtx);
	pthread_mutex_destroy(&t->acquisition_mtx);
	pthread_mutex_destroy(&t->codec_select_client_mtx);
	pthread_mutex_destroy(&t->codec_id_mtx);
	free(t->bluez_dbus_owner);
	free(t->bluez_dbus_path);
	free(t);
}

struct ba_transport_pcm *ba_transport_pcm_ref(struct ba_transport_pcm *pcm) {
	ba_transport_ref(pcm->t);
	return pcm;
}

void ba_transport_pcm_unref(struct ba_transport_pcm *pcm) {
	ba_transport_unref(pcm->t);
}

int ba_transport_select_codec_a2dp(
		struct ba_transport *t,
		const struct a2dp_sep *sep) {

	if (!(t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP))
		return errno = ENOTSUP, -1;

	pthread_mutex_lock(&t->codec_id_mtx);

	/* the same codec with the same configuration already selected */
	if (t->codec_id == sep->codec_id &&
			memcmp(&sep->configuration, &t->a2dp.configuration, sep->capabilities_size) == 0)
		goto final;

	/* A2DP codec selection is in fact a transport recreation - new transport
	 * with new codec is created and the current one is released. Since normally
	 * the storage is updated only when the transport is released, we need to
	 * update it manually here. Otherwise, new transport might be created with
	 * stale storage data. */
	storage_pcm_data_update(&t->a2dp.pcm);
	storage_pcm_data_update(&t->a2dp.pcm_bc);

	GError *err = NULL;
	if (!bluez_a2dp_set_configuration(t->a2dp.bluez_dbus_sep_path, sep, &err)) {
		error("Couldn't set A2DP configuration: %s", err->message);
		pthread_mutex_unlock(&t->codec_id_mtx);
		g_error_free(err);
		return errno = EIO, -1;
	}

final:
	pthread_mutex_unlock(&t->codec_id_mtx);
	return 0;
}

int ba_transport_select_codec_sco(
		struct ba_transport *t,
		uint16_t codec_id) {

#if !ENABLE_MSBC
	(void)codec_id;
#endif

	switch (t->profile) {
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HFP_AG:
#if ENABLE_MSBC

		/* with oFono back-end we have no access to RFCOMM */
		if (t->sco.rfcomm == NULL)
			return errno = ENOTSUP, -1;

		/* Lock the mutex because we are about to change the codec ID. The codec
		 * ID itself will be set by the RFCOMM thread. The RFCOMM thread and the
		 * current one will be synchronized by the RFCOMM codec selection
		 * condition variable. */
		pthread_mutex_lock(&t->codec_id_mtx);

		struct ba_rfcomm * const r = t->sco.rfcomm;
		enum ba_rfcomm_signal rfcomm_signal;

		/* codec already selected, skip switching */
		if (t->codec_id == codec_id)
			goto final;

		switch (codec_id) {
		case HFP_CODEC_CVSD:
			rfcomm_signal = BA_RFCOMM_SIGNAL_HFP_SET_CODEC_CVSD;
			break;
		case HFP_CODEC_MSBC:
			rfcomm_signal = BA_RFCOMM_SIGNAL_HFP_SET_CODEC_MSBC;
			break;
		default:
			g_assert_not_reached();
		}

		/* stop transport IO threads */
		ba_transport_stop(t);

		ba_transport_pcms_full_lock(t);
		/* release ongoing PCM connections */
		ba_transport_pcm_release(&t->sco.spk_pcm);
		ba_transport_pcm_release(&t->sco.mic_pcm);
		ba_transport_pcms_full_unlock(t);

		r->codec_selection_done = false;
		/* delegate set codec to RFCOMM thread */
		ba_rfcomm_send_signal(r, rfcomm_signal);

		while (!r->codec_selection_done)
			pthread_cond_wait(&r->codec_selection_cond, &t->codec_id_mtx);

		if (t->codec_id != codec_id) {
			pthread_mutex_unlock(&t->codec_id_mtx);
			return errno = EIO, -1;
		}

final:
		pthread_mutex_unlock(&t->codec_id_mtx);
		break;
#endif

	case BA_TRANSPORT_PROFILE_HSP_HS:
	case BA_TRANSPORT_PROFILE_HSP_AG:
	default:
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static void ba_transport_set_codec_a2dp(
		struct ba_transport *t,
		uint16_t codec_id) {
	switch (codec_id) {
	case A2DP_CODEC_SBC:
		a2dp_sbc_transport_init(t);
		break;
#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12:
		a2dp_mpeg_transport_init(t);
		break;
#endif
#if ENABLE_AAC
	case A2DP_CODEC_MPEG24:
		a2dp_aac_transport_init(t);
		break;
#endif
#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX:
		a2dp_aptx_transport_init(t);
		break;
#endif
#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD:
		a2dp_aptx_hd_transport_init(t);
		break;
#endif
#if ENABLE_FASTSTREAM
	case A2DP_CODEC_VENDOR_FASTSTREAM:
		a2dp_faststream_transport_init(t);
		break;
#endif
#if ENABLE_LC3PLUS
	case A2DP_CODEC_VENDOR_LC3PLUS:
		a2dp_lc3plus_transport_init(t);
		break;
#endif
#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC:
		a2dp_ldac_transport_init(t);
		break;
#endif
	default:
		error("Unsupported A2DP codec: %#x", codec_id);
		g_assert_not_reached();
	}
}

static void ba_transport_set_codec_sco(
		struct ba_transport *t,
		uint16_t codec_id) {

	t->sco.spk_pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->sco.spk_pcm.channels = 1;

	t->sco.mic_pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->sco.mic_pcm.channels = 1;

	switch (codec_id) {
	case HFP_CODEC_UNDEFINED:
		t->sco.spk_pcm.sampling = 0;
		t->sco.mic_pcm.sampling = 0;
		break;
	case HFP_CODEC_CVSD:
		t->sco.spk_pcm.sampling = 8000;
		t->sco.mic_pcm.sampling = 8000;
		break;
#if ENABLE_MSBC
	case HFP_CODEC_MSBC:
		t->sco.spk_pcm.sampling = 16000;
		t->sco.mic_pcm.sampling = 16000;
		break;
#endif
	default:
		debug("Unsupported SCO codec: %#x", codec_id);
		g_assert_not_reached();
	}

	if (t->sco.spk_pcm.ba_dbus_exported)
		bluealsa_dbus_pcm_update(&t->sco.spk_pcm,
				BA_DBUS_PCM_UPDATE_SAMPLING |
				BA_DBUS_PCM_UPDATE_CODEC |
				BA_DBUS_PCM_UPDATE_DELAY_ADJUSTMENT);

	if (t->sco.mic_pcm.ba_dbus_exported)
		bluealsa_dbus_pcm_update(&t->sco.mic_pcm,
				BA_DBUS_PCM_UPDATE_SAMPLING |
				BA_DBUS_PCM_UPDATE_CODEC |
				BA_DBUS_PCM_UPDATE_DELAY_ADJUSTMENT);

}

uint16_t ba_transport_get_codec(
		const struct ba_transport *t) {
	pthread_mutex_lock(MUTABLE(&t->codec_id_mtx));
	uint16_t codec_id = t->codec_id;
	pthread_mutex_unlock(MUTABLE(&t->codec_id_mtx));
	return codec_id;
}

void ba_transport_set_codec(
		struct ba_transport *t,
		uint16_t codec_id) {

	pthread_mutex_lock(&t->codec_id_mtx);

	bool changed = t->codec_id != codec_id;
	t->codec_id = codec_id;

	pthread_mutex_unlock(&t->codec_id_mtx);

	if (!changed)
		return;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		ba_transport_set_codec_a2dp(t, codec_id);
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		ba_transport_set_codec_sco(t, codec_id);

}

/**
 * Start transport IO threads.
 *
 * This function requires transport threads to be in the IDLE state. If any of
 * the threads is not in the IDLE state, then this function will fail and the
 * errno will be set to EINVAL.
 *
 * @param t Transport structure.
 * @return On success this function returns 0. Otherwise -1 is returned and
 *   errno is set to indicate the error. */
int ba_transport_start(struct ba_transport *t) {

	/* For A2DP Source profile only, it is possible that BlueZ will
	 * activate the transport following a D-Bus "Acquire" request before the
	 * client thread has completed the acquisition procedure by initializing
	 * the I/O threads state. So in that case we must ensure that the
	 * acquisition procedure is not still in progress before we check the
	 * threads' state. */
	if (t->profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		pthread_mutex_lock(&t->acquisition_mtx);

	pthread_mutex_lock(&t->thread_enc.mutex);
	bool is_enc_idle = t->thread_enc.state == BA_TRANSPORT_THREAD_STATE_IDLE;
	pthread_mutex_unlock(&t->thread_enc.mutex);
	pthread_mutex_lock(&t->thread_dec.mutex);
	bool is_dec_idle = t->thread_dec.state == BA_TRANSPORT_THREAD_STATE_IDLE;
	pthread_mutex_unlock(&t->thread_dec.mutex);

	if (t->profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		pthread_mutex_unlock(&t->acquisition_mtx);

	if (!is_enc_idle || !is_dec_idle)
		return errno = EINVAL, -1;

	debug("Starting transport: %s", ba_transport_debug_name(t));

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		switch (ba_transport_get_codec(t)) {
		case A2DP_CODEC_SBC:
			return a2dp_sbc_transport_start(t);
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return a2dp_mpeg_transport_start(t);
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return a2dp_aac_transport_start(t);
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return a2dp_aptx_transport_start(t);
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return a2dp_aptx_hd_transport_start(t);
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return a2dp_faststream_transport_start(t);
#endif
#if ENABLE_LC3PLUS
		case A2DP_CODEC_VENDOR_LC3PLUS:
			return a2dp_lc3plus_transport_start(t);
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return a2dp_ldac_transport_start(t);
#endif
		}

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		return sco_transport_start(t);

	g_assert_not_reached();
	return -1;
}

/**
 * Schedule transport IO threads cancellation. */
static int ba_transport_stop_async(struct ba_transport *t) {

#if DEBUG
	/* Assert that we were called with the lock held, so we
	 * can safely check and modify the stopping flag. */
	g_assert_cmpint(pthread_mutex_trylock(&t->bt_fd_mtx), !=, 0);
#endif

	if (t->stopping)
		return 0;

	t->stopping = true;

	/* Unlock the mutex before updating thread states. This is necessary to avoid
	 * lock order inversion with the code in the ba_transport_thread_bt_acquire()
	 * function. It is safe to do so, because we have already set the stopping
	 * flag, so the transport_threads_cancel() function will not be called before
	 * we acquire the lock again. */
	pthread_mutex_unlock(&t->bt_fd_mtx);

	ba_transport_thread_state_set_stopping(&t->thread_enc);
	ba_transport_thread_state_set_stopping(&t->thread_dec);

	pthread_mutex_lock(&t->bt_fd_mtx);

	if (transport_thread_manager_send_command(t, BA_TRANSPORT_THREAD_MANAGER_CANCEL_THREADS) != 0)
		return -1;

	return 0;
}

/**
 * Stop transport IO threads.
 *
 * This function waits for transport IO threads termination. It is not safe
 * to call it from IO thread itself - it will cause deadlock! */
int ba_transport_stop(struct ba_transport *t) {

	if (ba_transport_thread_state_check_terminated(&t->thread_enc) &&
			ba_transport_thread_state_check_terminated(&t->thread_dec))
		return 0;

	pthread_mutex_lock(&t->bt_fd_mtx);

	int rv;
	if ((rv = ba_transport_stop_async(t)) == -1)
		goto fail;

	while (t->stopping)
		pthread_cond_wait(&t->stopped, &t->bt_fd_mtx);

fail:
	pthread_mutex_unlock(&t->bt_fd_mtx);
	return rv;
}

/**
 * Stop transport IO threads if there are no PCM clients.
 *
 * This function does not wait for actual threads termination. It is safe to
 * call it even from the IO thread itself. Please note, that the check for
 * present PCM clients will happen after the keep-alive number of seconds. */
int ba_transport_stop_if_no_clients(struct ba_transport *t) {
	transport_thread_manager_send_command(t, BA_TRANSPORT_THREAD_MANAGER_CANCEL_IF_NO_CLIENTS);
	return 0;
}

int ba_transport_acquire(struct ba_transport *t) {

	bool acquired = false;
	int fd = -1;

	pthread_mutex_lock(&t->acquisition_mtx);

	pthread_mutex_lock(&t->bt_fd_mtx);

	/* If we are in the middle of IO threads stopping, wait until all resources
	 * are reclaimed, so we can acquire them in a clean way once more. */
	while (t->stopping)
		pthread_cond_wait(&t->stopped, &t->bt_fd_mtx);

	/* If BT socket file descriptor is still valid, we
	 * can safely reuse it (e.g. in a keep-alive mode). */
	if ((fd = t->bt_fd) != -1) {
		debug("Reusing BT socket: %d", fd);
		goto final;
	}

	/* Call transport specific acquire callback. */
	if ((fd = t->acquire(t)) != -1)
		acquired = true;

final:
	pthread_mutex_unlock(&t->bt_fd_mtx);

	if (acquired) {

		ba_transport_thread_state_set_idle(&t->thread_enc);
		ba_transport_thread_state_set_idle(&t->thread_dec);

		/* For SCO profiles we can start transport IO threads right away. There
		 * is no asynchronous signaling from BlueZ like with A2DP profiles. */
		if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
			if (ba_transport_start(t) == -1) {
				t->release(t);
				return -1;
			}
		}

	}

	pthread_mutex_unlock(&t->acquisition_mtx);

	return fd;
}

int ba_transport_release(struct ba_transport *t) {

	int ret = 0;

	pthread_mutex_lock(&t->bt_fd_mtx);

	/* If the transport has not been acquired, or it has been released already,
	 * there is no need to release it again. In fact, trying to release already
	 * closed transport will result in returning error message. */
	if (t->bt_fd == -1)
		goto final;

	ret = t->release(t);

final:
	pthread_mutex_unlock(&t->bt_fd_mtx);
	return ret;
}

int ba_transport_set_a2dp_state(
		struct ba_transport *t,
		enum bluez_a2dp_transport_state state) {
	switch (t->a2dp.state = state) {
	case BLUEZ_A2DP_TRANSPORT_STATE_PENDING:
		/* When transport is marked as pending, try to acquire transport, but only
		 * if we are handing A2DP sink profile. For source profile, transport has
		 * to be acquired by our controller (during the PCM open request). */
		if (t->profile == BA_TRANSPORT_PROFILE_A2DP_SINK)
			return ba_transport_acquire(t);
		return 0;
	case BLUEZ_A2DP_TRANSPORT_STATE_ACTIVE:
		return ba_transport_start(t);
	case BLUEZ_A2DP_TRANSPORT_STATE_IDLE:
	default:
		return ba_transport_stop(t);
	}
}

bool ba_transport_pcm_is_active(const struct ba_transport_pcm *pcm) {
	pthread_mutex_lock(MUTABLE(&pcm->mutex));
	bool active = pcm->fd != -1 && pcm->active;
	pthread_mutex_unlock(MUTABLE(&pcm->mutex));
	return active;
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

/**
 * Create transport thread. */
int ba_transport_thread_create(
		struct ba_transport_thread *th,
		ba_transport_thread_func th_func,
		const char *name,
		bool master) {

	struct ba_transport *t = th->t;
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

	if ((ret = pthread_create(&th->id, NULL, PTHREAD_FUNC(th_func), th)) != 0) {
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

/**
 * Transport IO thread cleanup function for pthread cleanup. */
void ba_transport_thread_cleanup(struct ba_transport_thread *th) {

	struct ba_transport *t = th->t;

	/* For proper functioning of the transport, all threads have to be
	 * operational. Therefore, if one of the threads is being cancelled,
	 * we have to cancel all other threads. */
	pthread_mutex_lock(&t->bt_fd_mtx);
	ba_transport_stop_async(t);
	pthread_mutex_unlock(&t->bt_fd_mtx);

	/* Release BT socket file descriptor duplicate created either in the
	 * ba_transport_thread_create() function or in the IO thread itself. */
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

	/* Remove reference which was taken by the ba_transport_thread_create(). */
	ba_transport_unref(t);
}
