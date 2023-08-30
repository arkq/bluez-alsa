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

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-rfcomm.h"
#include "ba-transport-pcm.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "hci.h"
#include "hfp.h"
#include "sco.h"
#include "storage.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"

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
		pthread_mutex_lock(&t->sco.pcm_spk.client_mtx);
		pthread_mutex_lock(&t->sco.pcm_mic.client_mtx);
		/* lock PCM data mutexes */
		pthread_mutex_lock(&t->sco.pcm_spk.mutex);
		pthread_mutex_lock(&t->sco.pcm_mic.mutex);
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
		pthread_mutex_unlock(&t->sco.pcm_spk.mutex);
		pthread_mutex_unlock(&t->sco.pcm_mic.mutex);
		pthread_mutex_unlock(&t->sco.pcm_spk.client_mtx);
		pthread_mutex_unlock(&t->sco.pcm_mic.client_mtx);
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
			if (t->sco.pcm_spk.fd == -1 && t->sco.pcm_mic.fd == -1)
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

	t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd);
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

	transport_pcm_init(&t->sco.pcm_spk,
			is_ag ? &t->thread_enc : &t->thread_dec,
			is_ag ? BA_TRANSPORT_PCM_MODE_SINK : BA_TRANSPORT_PCM_MODE_SOURCE);

	transport_pcm_init(&t->sco.pcm_mic,
			is_ag ? &t->thread_dec : &t->thread_enc,
			is_ag ? BA_TRANSPORT_PCM_MODE_SOURCE : BA_TRANSPORT_PCM_MODE_SINK);

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

	storage_pcm_data_sync(&t->sco.pcm_spk);
	storage_pcm_data_sync(&t->sco.pcm_mic);

	bluealsa_dbus_pcm_register(&t->sco.pcm_spk);
	bluealsa_dbus_pcm_register(&t->sco.pcm_mic);

	if (rfcomm_fd != -1) {
		if ((t->sco.rfcomm = ba_rfcomm_new(t, rfcomm_fd)) == NULL)
			goto fail;
	}

	return t;

fail:
	err = errno;
	bluealsa_dbus_pcm_unregister(&t->sco.pcm_spk);
	bluealsa_dbus_pcm_unregister(&t->sco.pcm_mic);
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
		bluealsa_dbus_pcm_unregister(&t->sco.pcm_spk);
		bluealsa_dbus_pcm_unregister(&t->sco.pcm_mic);
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
		ba_transport_pcm_release(&t->sco.pcm_spk);
		ba_transport_pcm_release(&t->sco.pcm_mic);
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
		storage_pcm_data_update(&t->sco.pcm_spk);
		storage_pcm_data_update(&t->sco.pcm_mic);
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
		transport_pcm_free(&t->sco.pcm_spk);
		transport_pcm_free(&t->sco.pcm_mic);
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
		ba_transport_pcm_release(&t->sco.pcm_spk);
		ba_transport_pcm_release(&t->sco.pcm_mic);
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
		a2dp_transport_init(t);
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		sco_transport_init(t);

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

	bool is_enc_idle = ba_transport_thread_state_check_idle(&t->thread_enc);
	bool is_dec_idle = ba_transport_thread_state_check_idle(&t->thread_dec);

	if (t->profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		pthread_mutex_unlock(&t->acquisition_mtx);

	if (!is_enc_idle || !is_dec_idle)
		return errno = EINVAL, -1;

	debug("Starting transport: %s", ba_transport_debug_name(t));

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return a2dp_transport_start(t);

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		return sco_transport_start(t);

	g_assert_not_reached();
	return -1;
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
 * Schedule transport IO threads cancellation. */
int ba_transport_stop_async(struct ba_transport *t) {

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
