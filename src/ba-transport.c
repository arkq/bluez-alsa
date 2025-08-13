/*
 * BlueALSA - ba-transport.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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

#include <alsa/asoundlib.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-rfcomm.h"
#include "ba-transport-pcm.h"
#include "ba-config.h"
#include "ble-midi.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "hci.h"
#include "hfp.h"
#include "midi.h"
#include "sco.h"
#include "storage.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"

static int ba_transport_pcms_full_lock(struct ba_transport *t) {
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		/* lock client mutexes first to avoid deadlock */
		pthread_mutex_lock(&t->media.pcm.client_mtx);
		pthread_mutex_lock(&t->media.pcm_bc.client_mtx);
		/* lock PCM data mutexes */
		pthread_mutex_lock(&t->media.pcm.mutex);
		pthread_mutex_lock(&t->media.pcm_bc.mutex);
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
		pthread_mutex_unlock(&t->media.pcm.mutex);
		pthread_mutex_unlock(&t->media.pcm_bc.mutex);
		pthread_mutex_unlock(&t->media.pcm.client_mtx);
		pthread_mutex_unlock(&t->media.pcm_bc.client_mtx);
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

static void transport_threads_cancel(struct ba_transport *t) {

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		ba_transport_pcm_stop(&t->media.pcm);
		ba_transport_pcm_stop(&t->media.pcm_bc);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		ba_transport_pcm_stop(&t->sco.pcm_spk);
		ba_transport_pcm_stop(&t->sco.pcm_mic);
	}

	pthread_mutex_lock(&t->bt_fd_mtx);
	t->stopping = false;
	pthread_mutex_unlock(&t->bt_fd_mtx);

	pthread_cond_broadcast(&t->stopped_cond);

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
			if (t->media.pcm.fd == -1 && t->media.pcm_bc.fd == -1)
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
		if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
			ba_transport_pcm_state_set_stopping(&t->media.pcm);
			ba_transport_pcm_state_set_stopping(&t->media.pcm_bc);
		}
		else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
			ba_transport_pcm_state_set_stopping(&t->sco.pcm_spk);
			ba_transport_pcm_state_set_stopping(&t->sco.pcm_mic);
		}
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
	pthread_cond_init(&t->stopped_cond, NULL);

	t->bt_fd = -1;

	t->thread_manager_thread_id = config.main_thread;
	t->thread_manager_pipe[0] = -1;
	t->thread_manager_pipe[1] = -1;

	if (pipe(t->thread_manager_pipe) == -1)
		goto fail;

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
			t->media.state == BLUEZ_MEDIA_TRANSPORT_STATE_PENDING ? "TryAcquire" : "Acquire");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL ||
			g_dbus_message_to_gerror(rep, &err))
		goto fail;

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

	if (ioctl(fd, TIOCOUTQ, &t->media.bt_fd_coutq_init) == -1)
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
	if (t->media.state != BLUEZ_MEDIA_TRANSPORT_STATE_IDLE &&
			t->bluez_dbus_owner != NULL) {

		debug("Releasing A2DP transport: %d", t->bt_fd);

		msg = g_dbus_message_new_method_call(t->bluez_dbus_owner, t->bluez_dbus_path,
				BLUEZ_IFACE_MEDIA_TRANSPORT, "Release");

		if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
						G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL ||
				g_dbus_message_to_gerror(rep, &err)) {
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
		else {

			/* When A2DP transport is released, its state is set to idle. Such
			 * change is notified by BlueZ via D-Bus asynchronous D-Bus signal.
			 * We have to wait for the state change here, because otherwise we
			 * might receive this state change signal in the middle of transport
			 * acquisition, which would lead us to an undefined state. */
			while (t->media.state != BLUEZ_MEDIA_TRANSPORT_STATE_IDLE)
				pthread_cond_wait(&t->media.state_changed_cond, &t->bt_fd_mtx);

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
		const struct a2dp_sep *sep,
		const void *configuration) {

	const bool is_sink = profile & BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t;
	int err = 0;

	if ((t = transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->profile = profile;
	t->codec_id = sep->config.codec_id;

	pthread_cond_init(&t->media.state_changed_cond, NULL);
	t->media.state = BLUEZ_MEDIA_TRANSPORT_STATE_IDLE;

	t->media.sep = sep;
	memcpy(&t->media.configuration, configuration, sep->config.caps_size);

	t->acquire = transport_acquire_bt_a2dp;
	t->release = transport_release_bt_a2dp;

	err |= transport_pcm_init(&t->media.pcm,
			is_sink ? BA_TRANSPORT_PCM_MODE_SOURCE : BA_TRANSPORT_PCM_MODE_SINK,
			t, true);

	err |= transport_pcm_init(&t->media.pcm_bc,
			is_sink ?  BA_TRANSPORT_PCM_MODE_SINK : BA_TRANSPORT_PCM_MODE_SOURCE,
			t, false);

	if (err != 0)
		goto fail;

	/* do codec-specific initialization */
	if (sep->transport_init(t) != 0) {
		errno = EINVAL;
		goto fail;
	}

	if ((errno = pthread_create(&t->thread_manager_thread_id,
			NULL, PTHREAD_FUNC(transport_thread_manager), t)) != 0) {
		t->thread_manager_thread_id = config.main_thread;
		goto fail;
	}

	storage_pcm_data_sync(&t->media.pcm);
	storage_pcm_data_sync(&t->media.pcm_bc);

	if (t->media.pcm.channels > 0)
		bluealsa_dbus_pcm_register(&t->media.pcm);
	if (t->media.pcm_bc.channels > 0)
		bluealsa_dbus_pcm_register(&t->media.pcm_bc);

	return t;

fail:
	err = errno;
	ba_transport_unref(t);
	errno = err;
	return NULL;
}

__attribute__ ((weak))
int transport_acquire_bt_sco(struct ba_transport *t) {

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

	const uint32_t codec_id = ba_transport_get_codec(t);
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
	struct ba_transport *t;
	int err = 0;

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
	/* In case of the HSP and HFP without codec selection support,
	 * there is no other option than the CVSD codec. */
	t->codec_id = HFP_CODEC_CVSD;

#if ENABLE_HFP_CODEC_SELECTION
	/* Only HFP supports codec selection. */
	if (profile & BA_TRANSPORT_PROFILE_MASK_HFP &&
			/* Check whether support for codecs other than the
			 * CVSD is possible with the underlying adapter. */
			BA_TEST_ESCO_SUPPORT(device->a)) {
# if ENABLE_MSBC
		if (config.hfp.codecs.msbc)
			t->codec_id = HFP_CODEC_UNDEFINED;
# endif
# if ENABLE_LC3_SWB
		if (config.hfp.codecs.lc3_swb)
			t->codec_id = HFP_CODEC_UNDEFINED;
# endif
	}
#endif

	t->acquire = transport_acquire_bt_sco;
	t->release = transport_release_bt_sco;

	err |= transport_pcm_init(&t->sco.pcm_spk,
			is_ag ? BA_TRANSPORT_PCM_MODE_SINK : BA_TRANSPORT_PCM_MODE_SOURCE,
			t, true);

	err |= transport_pcm_init(&t->sco.pcm_mic,
			is_ag ? BA_TRANSPORT_PCM_MODE_SOURCE : BA_TRANSPORT_PCM_MODE_SINK,
			t, false);

	if (err != 0)
		goto fail;

	if (sco_transport_init(t) != 0) {
		errno = EINVAL;
		goto fail;
	}

	if ((errno = pthread_create(&t->thread_manager_thread_id,
			NULL, PTHREAD_FUNC(transport_thread_manager), t)) != 0) {
		t->thread_manager_thread_id = config.main_thread;
		goto fail;
	}

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

#if ENABLE_MIDI

static int transport_acquire_bt_midi(struct ba_transport *t) {
	return midi_transport_alsa_seq_create(t);
}

static int transport_release_bt_midi(struct ba_transport *t) {

	midi_transport_alsa_seq_delete(t);

	if (t->midi.ble_fd_write != -1) {
		debug("Releasing BLE-MIDI write link: %d", t->midi.ble_fd_write);
		close(t->midi.ble_fd_write);
		t->midi.ble_fd_write = -1;
	}

	if (t->midi.ble_fd_notify != -1) {
		debug("Releasing BLE-MIDI notify link: %d", t->midi.ble_fd_notify);
		close(t->midi.ble_fd_notify);
		t->midi.ble_fd_notify = -1;
	}

	return 0;
}

struct ba_transport *ba_transport_new_midi(
		struct ba_device *device,
		enum ba_transport_profile profile,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t;
	if ((t = transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->profile = profile;

	t->midi.seq_port = -1;
	t->midi.seq_queue = -1;
	t->midi.ble_fd_write = -1;
	t->midi.ble_fd_notify = -1;

	int err;
	if ((err = snd_midi_event_new(1024, &t->midi.seq_parser)) < 0) {
		error("Couldn't create MIDI event decoder: %s", snd_strerror(err));
		goto fail;
	}

	/* Disable MIDI running status generated by the decoder. */
	snd_midi_event_no_status(t->midi.seq_parser, 1);

	t->acquire = transport_acquire_bt_midi;
	t->release = transport_release_bt_midi;

	return t;

fail:
	ba_transport_unref(t);
	errno = -err;
	return NULL;
}

#endif

#if DEBUG
/**
 * Get BlueALSA transport type debug name.
 *
 * It is guaranteed that this function will not lock/unlock any mutex at a
 * cost of potential race condition when retrieving the name. This function
 * is intended to be used for debugging purposes only.
 *
 * @param t Transport structure.
 * @return Human-readable string. */
__attribute__ ((no_sanitize("thread")))
const char *ba_transport_debug_name(
		const struct ba_transport *t) {
	switch (t->profile) {
	case BA_TRANSPORT_PROFILE_NONE:
		return "NONE";
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		return t->media.sep->name;
	case BA_TRANSPORT_PROFILE_HFP_HF:
		switch (t->codec_id) {
		case HFP_CODEC_UNDEFINED:
			return "HFP Hands-Free (...)";
		case HFP_CODEC_CVSD:
			return "HFP Hands-Free (CVSD)";
		case HFP_CODEC_MSBC:
			return "HFP Hands-Free (mSBC)";
		case HFP_CODEC_LC3_SWB:
			return "HFP Hands-Free (LC3-SWB)";
		} break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
		switch (t->codec_id) {
		case HFP_CODEC_UNDEFINED:
			return "HFP Audio Gateway (...)";
		case HFP_CODEC_CVSD:
			return "HFP Audio Gateway (CVSD)";
		case HFP_CODEC_MSBC:
			return "HFP Audio Gateway (mSBC)";
		case HFP_CODEC_LC3_SWB:
			return "HFP Audio Gateway (LC3-SWB)";
		} break;
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "HSP Headset";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "HSP Audio Gateway";
#if ENABLE_MIDI
	case BA_TRANSPORT_PROFILE_MIDI:
		return "MIDI";
#endif
	}
	debug("Unknown transport: profile:%#x codec:%#x", t->profile, t->codec_id);
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
		bluealsa_dbus_pcm_unregister(&t->media.pcm);
		bluealsa_dbus_pcm_unregister(&t->media.pcm_bc);
		/* Make sure that the transport A2DP state is set to idle
		 * prior to stopping the IO threads. */
		ba_transport_set_media_state(t, BLUEZ_MEDIA_TRANSPORT_STATE_IDLE);
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
		ba_transport_pcm_release(&t->media.pcm);
		ba_transport_pcm_release(&t->media.pcm_bc);
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

	debug("Freeing transport: %s", ba_transport_debug_name(t));
	g_assert_cmpint(ref_count, ==, 0);

	if (t->bt_fd != -1)
		close(t->bt_fd);

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		storage_pcm_data_update(&t->media.pcm);
		storage_pcm_data_update(&t->media.pcm_bc);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		storage_pcm_data_update(&t->sco.pcm_spk);
		storage_pcm_data_update(&t->sco.pcm_mic);
	}

	ba_device_unref(d);

#if DEBUG
	/* If IO threads are not terminated yet, we can not go any further.
	 * Such situation may occur when the transport is about to be freed from one
	 * of the transport IO threads. The transport thread cleanup function sends
	 * a command to the manager to terminate all other threads. In such case, we
	 * will stuck here, because we are about to wait for the transport thread
	 * manager to terminate. But the manager will not terminate, because it is
	 * waiting for a transport thread to terminate - which is us... */
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		g_assert_cmpint(ba_transport_pcm_state_check_terminated(&t->media.pcm), ==, true);
		g_assert_cmpint(ba_transport_pcm_state_check_terminated(&t->media.pcm_bc), ==, true);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		g_assert_cmpint(ba_transport_pcm_state_check_terminated(&t->sco.pcm_spk), ==, true);
		g_assert_cmpint(ba_transport_pcm_state_check_terminated(&t->sco.pcm_mic), ==, true);
	}
#endif

	if (!pthread_equal(t->thread_manager_thread_id, config.main_thread)) {
		transport_thread_manager_send_command(t, BA_TRANSPORT_THREAD_MANAGER_TERMINATE);
		pthread_join(t->thread_manager_thread_id, NULL);
	}

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		transport_pcm_free(&t->media.pcm);
		transport_pcm_free(&t->media.pcm_bc);
		pthread_cond_destroy(&t->media.state_changed_cond);
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
#if ENABLE_MIDI
	else if (t->profile & BA_TRANSPORT_PROFILE_MIDI) {
		if (t->midi.seq_parser != NULL)
			snd_midi_event_free(t->midi.seq_parser);
		ble_midi_decode_free(&t->midi.ble_decoder);
	}
#endif

	if (t->thread_manager_pipe[0] != -1)
		close(t->thread_manager_pipe[0]);
	if (t->thread_manager_pipe[1] != -1)
		close(t->thread_manager_pipe[1]);

	pthread_cond_destroy(&t->stopped_cond);
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
		const struct a2dp_sep_config *remote_sep_cfg,
		const void *configuration) {

#if DEBUG
	/* Assert that we were called with the codec selection mutex locked. */
	g_assert_cmpint(pthread_mutex_trylock(&t->codec_select_client_mtx), !=, 0);
#endif

	/* the same codec with the same configuration already selected */
	if (remote_sep_cfg->codec_id == t->codec_id &&
			memcmp(configuration, &t->media.configuration, remote_sep_cfg->caps_size) == 0)
		goto final;

	/* A2DP codec selection is in fact a transport recreation - new transport
	 * with new codec is created and the current one is released. Since normally
	 * the storage is updated only when the transport is released, we need to
	 * update it manually here. Otherwise, new transport might be created with
	 * stale storage data. */
	storage_pcm_data_update(&t->media.pcm);
	storage_pcm_data_update(&t->media.pcm_bc);

	GError *err = NULL;
	if (!bluez_a2dp_set_configuration(t->media.bluez_dbus_sep_path,
				remote_sep_cfg, configuration, &err)) {
		error("Couldn't set A2DP configuration: %s", err->message);
		g_error_free(err);
		return errno = EIO, -1;
	}

final:
	return 0;
}

int ba_transport_select_codec_sco(
		struct ba_transport *t,
		uint8_t codec_id) {

#if DEBUG
	/* Assert that we were called with the codec selection mutex locked. */
	g_assert_cmpint(pthread_mutex_trylock(&t->codec_select_client_mtx), !=, 0);
#endif

	switch (t->profile) {
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HFP_AG:
#if ENABLE_HFP_CODEC_SELECTION

		/* with oFono back-end we have no access to RFCOMM */
		if (t->sco.rfcomm == NULL)
			return errno = ENOTSUP, -1;

		struct ba_rfcomm * const r = t->sco.rfcomm;
		enum ba_rfcomm_signal rfcomm_signal;

		/* codec already selected, skip switching */
		if (t->codec_id == codec_id)
			return 0;

		/* The codec ID itself will be set by the RFCOMM thread. The
		 * RFCOMM thread and the current one will be synchronized by
		 * the RFCOMM codec selection condition variable. */

		switch (codec_id) {
		case HFP_CODEC_CVSD:
			rfcomm_signal = BA_RFCOMM_SIGNAL_HFP_SET_CODEC_CVSD;
			break;
		case HFP_CODEC_MSBC:
			rfcomm_signal = BA_RFCOMM_SIGNAL_HFP_SET_CODEC_MSBC;
			break;
		case HFP_CODEC_LC3_SWB:
			rfcomm_signal = BA_RFCOMM_SIGNAL_HFP_SET_CODEC_LC3_SWB;
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
			pthread_cond_wait(&r->codec_selection_cond, &t->codec_select_client_mtx);

		if (t->codec_id != codec_id)
			return errno = EIO, -1;

#else
		(void)codec_id;
#endif
		return 0;
	case BA_TRANSPORT_PROFILE_HSP_HS:
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return errno = ENOTSUP, -1;
	default:
		g_assert_not_reached();
	}

}

uint32_t ba_transport_get_codec(
		const struct ba_transport *t) {
	pthread_mutex_lock(MUTABLE(&t->codec_id_mtx));
	uint32_t codec_id = t->codec_id;
	pthread_mutex_unlock(MUTABLE(&t->codec_id_mtx));
	return codec_id;
}

void ba_transport_set_codec(
		struct ba_transport *t,
		uint32_t codec_id) {

	pthread_mutex_lock(&t->codec_id_mtx);

	bool changed = t->codec_id != codec_id;
	t->codec_id = codec_id;

	pthread_mutex_unlock(&t->codec_id_mtx);

	if (!changed)
		return;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		t->media.sep->transport_init(t);
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

#if ENABLE_MIDI
	if (t->profile & BA_TRANSPORT_PROFILE_MIDI)
		return midi_transport_start(t);
#endif

	/* For A2DP Source profile only, it is possible that BlueZ will
	 * activate the transport following a D-Bus "Acquire" request before the
	 * client thread has completed the acquisition procedure by initializing
	 * the I/O threads state. So in that case we must ensure that the
	 * acquisition procedure is not still in progress before we check the
	 * threads' state. */
	if (t->profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		pthread_mutex_lock(&t->acquisition_mtx);

	bool is_enc_idle = false, is_dec_idle = false;
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		is_enc_idle = ba_transport_pcm_state_check_idle(&t->media.pcm);
		is_dec_idle = ba_transport_pcm_state_check_idle(&t->media.pcm_bc);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		is_enc_idle = ba_transport_pcm_state_check_idle(&t->sco.pcm_spk);
		is_dec_idle = ba_transport_pcm_state_check_idle(&t->sco.pcm_mic);
	}

	if (t->profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		pthread_mutex_unlock(&t->acquisition_mtx);

	if (!is_enc_idle || !is_dec_idle)
		return errno = EINVAL, -1;

	debug("Starting transport: %s", ba_transport_debug_name(t));

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return t->media.sep->transport_start(t);

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

#if ENABLE_MIDI
	if (t->profile & BA_TRANSPORT_PROFILE_MIDI)
		return midi_transport_stop(t);
#endif

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		if (ba_transport_pcm_state_check_terminated(&t->media.pcm) &&
				ba_transport_pcm_state_check_terminated(&t->media.pcm_bc))
			return 0;
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		if (ba_transport_pcm_state_check_terminated(&t->sco.pcm_spk) &&
				ba_transport_pcm_state_check_terminated(&t->sco.pcm_mic))
			return 0;
	}

	pthread_mutex_lock(&t->bt_fd_mtx);

	int rv;
	if ((rv = ba_transport_stop_async(t)) == -1)
		goto fail;

	while (t->stopping)
		pthread_cond_wait(&t->stopped_cond, &t->bt_fd_mtx);

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
	 * lock order inversion with the code in the ba_transport_pcm_bt_acquire()
	 * function. It is safe to do so, because we have already set the stopping
	 * flag, so the transport_threads_cancel() function will not be called before
	 * we acquire the lock again. */
	pthread_mutex_unlock(&t->bt_fd_mtx);

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		ba_transport_pcm_state_set_stopping(&t->media.pcm);
		ba_transport_pcm_state_set_stopping(&t->media.pcm_bc);
	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		ba_transport_pcm_state_set_stopping(&t->sco.pcm_spk);
		ba_transport_pcm_state_set_stopping(&t->sco.pcm_mic);
	}

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

#if ENABLE_MIDI
	if (t->profile & BA_TRANSPORT_PROFILE_MIDI)
		return t->acquire(t);
#endif

	bool acquired = false;
	int fd = -1;

	pthread_mutex_lock(&t->acquisition_mtx);

	pthread_mutex_lock(&t->bt_fd_mtx);

	/* If we are in the middle of IO threads stopping, wait until all resources
	 * are reclaimed, so we can acquire them in a clean way once more. */
	while (t->stopping)
		pthread_cond_wait(&t->stopped_cond, &t->bt_fd_mtx);

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

		if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
			ba_transport_pcm_state_set_idle(&t->media.pcm);
			ba_transport_pcm_state_set_idle(&t->media.pcm_bc);
		}
		else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
			ba_transport_pcm_state_set_idle(&t->sco.pcm_spk);
			ba_transport_pcm_state_set_idle(&t->sco.pcm_mic);
		}

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

#if ENABLE_MIDI
	if (t->profile & BA_TRANSPORT_PROFILE_MIDI)
		return t->release(t);
#endif

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

int ba_transport_set_media_state(
		struct ba_transport *t,
		enum bluez_media_transport_state state) {

	pthread_mutex_lock(&t->bt_fd_mtx);

	bool changed = t->media.state != state;
	t->media.state = state;

	pthread_mutex_unlock(&t->bt_fd_mtx);

	if (changed) {

		pthread_cond_signal(&t->media.state_changed_cond);

		switch (state) {
		case BLUEZ_MEDIA_TRANSPORT_STATE_IDLE:
			return ba_transport_stop(t);
		case BLUEZ_MEDIA_TRANSPORT_STATE_PENDING:
			/* When transport is marked as pending, try to acquire transport,
			 * but only if we are handing A2DP sink profile. For source profile,
			 * transport has to be acquired by our controller (during the PCM
			 * open request). */
			if (t->profile == BA_TRANSPORT_PROFILE_A2DP_SINK)
				return ba_transport_acquire(t);
			return 0;
		case BLUEZ_MEDIA_TRANSPORT_STATE_BROADCASTING:
			return -1;
		case BLUEZ_MEDIA_TRANSPORT_STATE_ACTIVE:
			return ba_transport_start(t);
		}

	}

	return 0;
}
