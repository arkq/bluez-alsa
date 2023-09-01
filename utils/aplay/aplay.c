/*
 * BlueALSA - aplay.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/nv.h"
#include "alsa-mixer.h"
#include "alsa-pcm.h"
#include "dbus.h"

enum volume_type {
	VOL_TYPE_AUTO,
	VOL_TYPE_MIXER,
	VOL_TYPE_SOFTWARE,
	VOL_TYPE_NONE,
};

struct io_worker {
	pthread_t thread;
	/* used BlueALSA PCM device */
	struct ba_pcm ba_pcm;
	/* file descriptor of PCM FIFO */
	int ba_pcm_fd;
	/* file descriptor of PCM control */
	int ba_pcm_ctrl_fd;
	/* opened playback PCM device */
	snd_pcm_t *snd_pcm;
	/* mixer for volume control */
	snd_mixer_t *snd_mixer;
	snd_mixer_elem_t *snd_mixer_elem;
	bool mixer_has_mute_switch;
	/* if true, playback is active */
	atomic_bool active;
	/* human-readable BT address */
	char addr[18];
};

static unsigned int verbose = 0;
static bool list_bt_devices = false;
static bool list_bt_pcms = false;
static const char *pcm_device = "default";
static enum volume_type volume_type = VOL_TYPE_AUTO;
static const char *mixer_device = "default";
static const char *mixer_elem_name = "Master";
static unsigned int mixer_elem_index = 0;
static bool ba_profile_a2dp = true;
static bool ba_addr_any = false;
static bdaddr_t *ba_addrs = NULL;
static size_t ba_addrs_count = 0;
static unsigned int pcm_buffer_time = 500000;
static unsigned int pcm_period_time = 100000;

/* local PCM muted state for software mute */
static bool pcm_muted = false;

static struct ba_dbus_ctx dbus_ctx;
static char dbus_ba_service[32] = BLUEALSA_SERVICE;

static struct ba_pcm *ba_pcms = NULL;
static size_t ba_pcms_count = 0;

static pthread_mutex_t single_playback_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool force_single_playback = false;

static pthread_rwlock_t workers_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct io_worker *workers = NULL;
static size_t workers_count = 0;
static size_t workers_size = 0;

static atomic_bool main_loop_on = true;
static void main_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);

	main_loop_on = false;
}

static int parse_bt_addresses(char *argv[], size_t count) {

	ba_addrs_count = count;
	if ((ba_addrs = malloc(sizeof(*ba_addrs) * ba_addrs_count)) == NULL)
		return -1;

	size_t i;
	for (i = 0; i < ba_addrs_count; i++) {
		if (str2ba(argv[i], &ba_addrs[i]) != 0)
			return errno = EINVAL, -1;
		if (bacmp(&ba_addrs[i], BDADDR_ANY) == 0)
			ba_addr_any = true;
	}

	return 0;
}

static const char *bluealsa_get_profile(const struct ba_pcm *pcm) {
	switch (pcm->transport) {
	case BA_PCM_TRANSPORT_A2DP_SOURCE:
	case BA_PCM_TRANSPORT_A2DP_SINK:
		return "A2DP";
	case BA_PCM_TRANSPORT_HFP_AG:
	case BA_PCM_TRANSPORT_HFP_HF:
	case BA_PCM_TRANSPORT_HSP_AG:
	case BA_PCM_TRANSPORT_HSP_HS:
		return "SCO";
	default:
		error("Unknown transport: %#x", pcm->transport);
		return "[...]";
	}
}

static snd_pcm_format_t bluealsa_get_snd_pcm_format(const struct ba_pcm *pcm) {
	switch (pcm->format) {
	case 0x0108:
		return SND_PCM_FORMAT_U8;
	case 0x8210:
		return SND_PCM_FORMAT_S16_LE;
	case 0x8318:
		return SND_PCM_FORMAT_S24_3LE;
	case 0x8418:
		return SND_PCM_FORMAT_S24_LE;
	case 0x8420:
		return SND_PCM_FORMAT_S32_LE;
	default:
		error("Unknown PCM format: %#x", pcm->format);
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

static void print_bt_device_list(void) {

	static const struct {
		const char *label;
		unsigned int mode;
	} section[2] = {
		{ "**** List of PLAYBACK Bluetooth Devices ****", BA_PCM_MODE_SINK },
		{ "**** List of CAPTURE Bluetooth Devices ****", BA_PCM_MODE_SOURCE },
	};

	const char *tmp;
	size_t i, ii;

	for (i = 0; i < ARRAYSIZE(section); i++) {
		printf("%s\n", section[i].label);
		for (ii = 0, tmp = ""; ii < ba_pcms_count; ii++) {

			struct ba_pcm *pcm = &ba_pcms[ii];
			struct bluez_device dev = { 0 };

			if (!(pcm->mode == section[i].mode))
				continue;

			if (strcmp(pcm->device_path, tmp) != 0) {
				tmp = ba_pcms[ii].device_path;

				DBusError err = DBUS_ERROR_INIT;
				if (dbus_bluez_get_device(dbus_ctx.conn, pcm->device_path, &dev, &err) == -1) {
					warn("Couldn't get BlueZ device properties: %s", err.message);
					dbus_error_free(&err);
				}

				char bt_addr[18];
				ba2str(&dev.bt_addr, bt_addr);

				printf("%s: %s [%s], %s%s\n",
						dev.hci_name, bt_addr, dev.name,
						dev.trusted ? "trusted " : "", dev.icon);

			}

			printf("  %s (%s): %s %d channel%s %d Hz\n",
					bluealsa_get_profile(pcm),
					pcm->codec.name,
					snd_pcm_format_name(bluealsa_get_snd_pcm_format(pcm)),
					pcm->channels, pcm->channels != 1 ? "s" : "",
					pcm->sampling);

		}
	}

}

static void print_bt_pcm_list(void) {

	DBusError err = DBUS_ERROR_INIT;
	struct bluez_device dev = { 0 };
	const char *tmp = "";
	size_t i;

	for (i = 0; i < ba_pcms_count; i++) {
		struct ba_pcm *pcm = &ba_pcms[i];

		if (strcmp(pcm->device_path, tmp) != 0) {
			tmp = ba_pcms[i].device_path;
			if (dbus_bluez_get_device(dbus_ctx.conn, pcm->device_path, &dev, &err) == -1) {
				warn("Couldn't get BlueZ device properties: %s", err.message);
				dbus_error_free(&err);
			}
		}

		char bt_addr[18];
		ba2str(&dev.bt_addr, bt_addr);

		printf(
				"bluealsa:DEV=%s,PROFILE=%s,SRV=%s\n"
				"    %s, %s%s, %s\n"
				"    %s (%s): %s %d channel%s %d Hz\n",
				bt_addr,
				pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP ? "a2dp" : "sco",
				dbus_ba_service,
				dev.name,
				dev.trusted ? "trusted " : "", dev.icon,
				pcm->mode == BA_PCM_MODE_SINK ? "playback" : "capture",
				bluealsa_get_profile(pcm),
				pcm->codec.name,
				snd_pcm_format_name(bluealsa_get_snd_pcm_format(pcm)),
				pcm->channels, pcm->channels != 1 ? "s" : "",
				pcm->sampling);

	}

}

static struct ba_pcm *ba_pcm_add(const struct ba_pcm *pcm) {
	struct ba_pcm *tmp;
	if ((tmp = realloc(ba_pcms, (ba_pcms_count + 1) * sizeof(*ba_pcms))) == NULL)
		return NULL;
	ba_pcms = tmp;
	memcpy(&ba_pcms[ba_pcms_count], pcm, sizeof(*ba_pcms));
	return &ba_pcms[ba_pcms_count++];
}

static struct ba_pcm *ba_pcm_get(const char *path) {
	for (size_t i = 0; i < ba_pcms_count; i++)
		if (strcmp(ba_pcms[i].pcm_path, path) == 0)
			return &ba_pcms[i];
	return NULL;
}

static void ba_pcm_remove(const char *path) {
	for (size_t i = 0; i < ba_pcms_count; i++)
		if (strcmp(ba_pcms[i].pcm_path, path) == 0) {
			memmove(&ba_pcms[i], &ba_pcms[i + 1],
					(ba_pcms_count - i - 1) * sizeof(*ba_pcms));
			ba_pcms_count--;
			break;
		}
}

static struct io_worker *get_active_io_worker(void) {

	struct io_worker *w = NULL;
	size_t i;

	pthread_rwlock_rdlock(&workers_lock);

	for (i = 0; i < workers_count; i++)
		if (workers[i].active) {
			w = &workers[i];
			break;
		}

	pthread_rwlock_unlock(&workers_lock);

	return w;
}

static int pause_device_player(const struct ba_pcm *ba_pcm) {

	DBusMessage *msg = NULL, *rep = NULL;
	DBusError err = DBUS_ERROR_INIT;
	char path[160];
	int ret = 0;

	snprintf(path, sizeof(path), "%s/player0", ba_pcm->device_path);
	msg = dbus_message_new_method_call("org.bluez", path, "org.bluez.MediaPlayer1", "Pause");

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn, msg,
					DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		warn("Couldn't pause player: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	debug("Requested playback pause");
	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return ret;
}

/**
 * Update BlueALSA PCM volume according to ALSA mixer element. */
static int io_worker_mixer_volume_sync_ba_pcm(
		struct io_worker *worker,
		struct ba_pcm *ba_pcm) {

	snd_mixer_elem_t *elem = worker->snd_mixer_elem;
	const int vmax = BA_PCM_VOLUME_MAX(ba_pcm);
	long long volume_db_sum = 0;
	bool muted = true;

	snd_mixer_selem_channel_id_t ch;
	for (ch = 0; snd_mixer_selem_has_playback_channel(elem, ch) == 1; ch++) {

		long ch_volume_db;
		int ch_switch = 1;

		int err;
		if ((err = snd_mixer_selem_get_playback_dB(elem, 0, &ch_volume_db)) != 0) {
			error("Couldn't get ALSA mixer playback dB level: %s", snd_strerror(err));
			return -1;
		}

		/* mute switch is an optional feature for a mixer element */
		if ((worker->mixer_has_mute_switch = snd_mixer_selem_has_playback_switch(elem))) {
			if ((err = snd_mixer_selem_get_playback_switch(elem, 0, &ch_switch)) != 0) {
				error("Couldn't get ALSA mixer playback switch: %s", snd_strerror(err));
				return -1;
			}
		}

		volume_db_sum += ch_volume_db;
		if (ch_switch == 1)
			muted = false;

	}

	/* Safety check for undefined behavior from
	 * out-of-bounds dB conversion. */
	assert(volume_db_sum <= 0LL);

	/* Convert dB to loudness using decibel formula and
	 * round to the nearest integer. */
	int volume = lround(pow(2, (0.01 * volume_db_sum / ch) / 10) * vmax);

	/* If mixer element does not support playback switch,
	 * use our global muted state. */
	if (!worker->mixer_has_mute_switch)
		muted = pcm_muted;

	ba_pcm->volume.ch1_muted = muted;
	ba_pcm->volume.ch1_volume = volume;
	ba_pcm->volume.ch2_muted = muted;
	ba_pcm->volume.ch2_volume = volume;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, ba_pcm, BLUEALSA_PCM_VOLUME, &err)) {
		error("Couldn't update BlueALSA source PCM: %s", err.message);
		dbus_error_free(&err);
		return -1;
	}

	return 0;
}

/**
 * Update ALSA mixer element according to BlueALSA PCM volume. */
static int io_worker_mixer_volume_sync_snd_mixer_elem(
		struct io_worker *worker,
		struct ba_pcm *ba_pcm) {

	/* skip update in case of software volume */
	if (ba_pcm->soft_volume)
		return 0;

	snd_mixer_elem_t *elem = worker->snd_mixer_elem;
	if (elem == NULL)
		return 0;

	/* User can connect BlueALSA PCM to mono, stereo or multi-channel output.
	 * For mono input (audio from BlueALSA PCM), case case is simple: we are
	 * changing all output channels at once. However, for stereo input it is
	 * not possible to know how to control left/right volume unless there is
	 * some kind of channel mapping. In order to simplify things, we will set
	 * all channels to the average left-right volume. */

	const int vmax = BA_PCM_VOLUME_MAX(ba_pcm);
	int volume = ba_pcm->volume.ch1_volume;
	int muted = ba_pcm->volume.ch1_muted;

	if (ba_pcm->channels > 1) {
		volume = (ba_pcm->volume.ch1_volume + ba_pcm->volume.ch2_volume) / 2;
		muted = ba_pcm->volume.ch1_muted || ba_pcm->volume.ch2_muted;
	}

	/* keep local muted state up to date */
	pcm_muted = muted;

	/* convert loudness to dB using decibel formula */
	long db = 10 * log2(1.0 * volume / vmax) * 100;

	int err;
	if ((err = snd_mixer_selem_set_playback_dB_all(elem, db, 0)) != 0) {
		error("Couldn't set ALSA mixer playback dB level: %s", snd_strerror(err));
		return -1;
	}

	/* mute switch is an optional feature for a mixer element */
	if (worker->mixer_has_mute_switch &&
			(err = snd_mixer_selem_set_playback_switch_all(elem, !muted)) != 0) {
		error("Couldn't set ALSA mixer playback mute switch: %s", snd_strerror(err));
		return -1;
	}

	return 0;
}

int io_worker_mixer_elem_callback(snd_mixer_elem_t *elem, unsigned int mask) {
	struct io_worker *worker = snd_mixer_elem_get_callback_private(elem);
	if (mask & SND_CTL_EVENT_MASK_VALUE)
		io_worker_mixer_volume_sync_ba_pcm(worker, &worker->ba_pcm);
	return 0;
}

/**
 * Setup volume synchronization between ALSA mixer and BlueALSA PCM. */
static int io_worker_mixer_volume_sync_setup(
		struct io_worker *worker) {

	/* skip setup in case of software volume */
	if (worker->ba_pcm.soft_volume)
		return 0;

	snd_mixer_elem_t *elem = worker->snd_mixer_elem;
	if (elem == NULL)
		return 0;

	debug("Setting up ALSA mixer volume synchronization");

	snd_mixer_elem_set_callback(elem, io_worker_mixer_elem_callback);
	snd_mixer_elem_set_callback_private(elem, worker);

	/* initial synchronization */
	io_worker_mixer_volume_sync_ba_pcm(worker, &worker->ba_pcm);

	return 0;
}

static void io_worker_routine_exit(struct io_worker *worker) {
	if (worker->ba_pcm_fd != -1) {
		close(worker->ba_pcm_fd);
		worker->ba_pcm_fd = -1;
	}
	if (worker->ba_pcm_ctrl_fd != -1) {
		close(worker->ba_pcm_ctrl_fd);
		worker->ba_pcm_ctrl_fd = -1;
	}
	if (worker->snd_pcm != NULL) {
		snd_pcm_close(worker->snd_pcm);
		worker->snd_pcm = NULL;
	}
	if (worker->snd_mixer != NULL) {
		snd_mixer_close(worker->snd_mixer);
		worker->snd_mixer_elem = NULL;
		worker->snd_mixer = NULL;
	}
	debug("Exiting IO worker %s", worker->addr);
}

static void *io_worker_routine(struct io_worker *w) {

	snd_pcm_format_t pcm_format = bluealsa_get_snd_pcm_format(&w->ba_pcm);
	ssize_t pcm_format_size = snd_pcm_format_size(pcm_format, 1);
	size_t pcm_1s_samples = w->ba_pcm.sampling * w->ba_pcm.channels;
	ffb_t buffer = { 0 };

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	pthread_cleanup_push(PTHREAD_CLEANUP(io_worker_routine_exit), w);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

	/* create buffer big enough to hold 100 ms of PCM data */
	if (ffb_init(&buffer, pcm_1s_samples / 10, pcm_format_size) == -1) {
		error("Couldn't create PCM buffer: %s", strerror(errno));
		goto fail;
	}

	DBusError err = DBUS_ERROR_INIT;

	/* initialize the PCM soft_volume setting */
	if (volume_type != VOL_TYPE_AUTO) {
		bool softvol = (volume_type == VOL_TYPE_SOFTWARE);
		debug("Setting BlueALSA source PCM volume mode: %s: %s",
				w->ba_pcm.pcm_path, softvol ? "software" : "pass-through");
		if (softvol != w->ba_pcm.soft_volume) {
			w->ba_pcm.soft_volume = softvol;
			if (!bluealsa_dbus_pcm_update(&dbus_ctx, &w->ba_pcm,
						BLUEALSA_PCM_SOFT_VOLUME, &err)) {
				error("Couldn't set BlueALSA source PCM volume mode: %s", err.message);
				dbus_error_free(&err);
				goto fail;
			}
		}
	}

	debug("Opening BlueALSA source PCM: %s", w->ba_pcm.pcm_path);
	if (!bluealsa_dbus_pcm_open(&dbus_ctx, w->ba_pcm.pcm_path,
				&w->ba_pcm_fd, &w->ba_pcm_ctrl_fd, &err)) {
		error("Couldn't open BlueALSA source PCM: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	/* Initialize the max read length to 10 ms. Later, when the PCM device
	 * will be opened, this value will be adjusted to one period size. */
	size_t pcm_max_read_len_init = pcm_1s_samples / 100 * pcm_format_size;
	size_t pcm_max_read_len = pcm_max_read_len_init;

	/* Track the lock state of the single playback mutex within this thread. */
	bool single_playback_mutex_locked = false;

	/* Intervals in seconds between consecutive PCM open retry attempts. */
	const unsigned int pcm_open_retry_intervals[] = { 1, 1, 2, 3, 5 };
	size_t pcm_open_retry_pcm_samples = 0;
	size_t pcm_open_retries = 0;

	size_t pause_retry_pcm_samples = pcm_1s_samples;
	size_t pause_retries = 0;

	int timeout = -1;

	debug("Starting IO loop");
	while (main_loop_on) {

		if (single_playback_mutex_locked) {
			pthread_mutex_unlock(&single_playback_mutex);
			single_playback_mutex_locked = false;
		}

		struct pollfd fds[16] = {{ w->ba_pcm_fd, POLLIN, 0 }};
		nfds_t nfds = 1;

		if (w->snd_mixer != NULL)
			nfds += snd_mixer_poll_descriptors_count(w->snd_mixer);

		if (nfds > ARRAYSIZE(fds)) {
			error("Poll FD array size exceeded: %zu > %zu", nfds, ARRAYSIZE(fds));
			goto fail;
		}

		if (w->snd_mixer != NULL)
			snd_mixer_poll_descriptors(w->snd_mixer, fds + 1, nfds - 1);

		/* Reading from the FIFO won't block unless there is an open connection
		 * on the writing side. However, the server does not open PCM FIFO until
		 * a transport is created. With the A2DP, the transport is created when
		 * some clients (BT device) requests audio transfer. */

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		int poll_rv = poll(fds, nfds, timeout);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		switch (poll_rv) {
		case -1:
			if (errno == EINTR)
				continue;
			error("IO loop poll error: %s", strerror(errno));
			goto fail;
		case 0:
			debug("BT device marked as inactive: %s", w->addr);
			pause_retry_pcm_samples = pcm_1s_samples;
			pause_retries = 0;
			w->active = false;
			timeout = -1;
			goto close_alsa;
		}

		if (w->snd_mixer != NULL)
			snd_mixer_handle_events(w->snd_mixer);

		size_t read_samples = 0;
		if (fds[0].revents & POLLIN) {

			ssize_t ret;
			size_t _in = MIN(pcm_max_read_len, ffb_blen_in(&buffer));
			if ((ret = read(w->ba_pcm_fd, buffer.tail, _in)) == -1) {
				if (errno == EINTR)
					continue;
				error("BlueALSA source PCM read error: %s", strerror(errno));
				goto fail;
			}

			read_samples = ret / pcm_format_size;
			if (ret % pcm_format_size != 0)
				warn("Invalid read from BlueALSA source PCM: %zd %% %zd != 0", ret, pcm_format_size);

		}
		else if (fds[0].revents & POLLHUP) {
			/* source PCM FIFO has been terminated on the writing side */
			debug("BlueALSA source PCM disconnected: %s", w->ba_pcm.pcm_path);
			break;
		}
		else if (fds[0].revents)
			error("Unexpected BlueALSA source PCM poll event: %#x", fds[0].revents);

		if (read_samples == 0)
			continue;

		/* If current worker is not active and the single playback mode was
		 * enabled, we have to check if there is any other active worker. */
		if (force_single_playback && !w->active) {

			/* Before checking active worker, we need to lock the single playback
			 * mutex. It is required to lock it, because the active state is changed
			 * in the worker thread after opening the PCM device, so we have to
			 * synchronize all threads at this point. */
			pthread_mutex_lock(&single_playback_mutex);
			single_playback_mutex_locked = true;

			if (get_active_io_worker() != NULL) {
				/* In order not to flood BT connection with AVRCP packets,
				 * we are going to send pause command every 0.5 second. */
				if (pause_retries < 5 &&
						(pause_retry_pcm_samples += read_samples) > pcm_1s_samples / 2) {
					if (pause_device_player(&w->ba_pcm) == -1)
						/* pause command does not work, stop further requests */
						pause_retries = 5;
					pause_retry_pcm_samples = 0;
					pause_retries++;
					timeout = 100;
				}
				continue;
			}

		}

		if (w->snd_pcm == NULL) {

			unsigned int buffer_time = pcm_buffer_time;
			unsigned int period_time = pcm_period_time;
			snd_pcm_uframes_t buffer_frames;
			snd_pcm_uframes_t period_frames;
			char *tmp;

			if (pcm_open_retries > 0) {
				/* After PCM open failure wait some time before retry. This can not be
				 * done with a sleep() call, because we have to drain PCM FIFO, so it
				 * will not have any stale data. */
				unsigned int interval = pcm_open_retries > ARRAYSIZE(pcm_open_retry_intervals) ?
					pcm_open_retry_intervals[ARRAYSIZE(pcm_open_retry_intervals) - 1] :
					pcm_open_retry_intervals[pcm_open_retries - 1];
				if ((pcm_open_retry_pcm_samples += read_samples) <= interval * pcm_1s_samples)
					continue;
			}

			debug("Opening ALSA playback PCM: name=%s channels=%u rate=%u",
					pcm_device, w->ba_pcm.channels, w->ba_pcm.sampling);
			if (alsa_pcm_open(&w->snd_pcm, pcm_device, pcm_format, w->ba_pcm.channels,
						w->ba_pcm.sampling, &buffer_time, &period_time, &tmp) != 0) {
				warn("Couldn't open ALSA playback PCM: %s", tmp);
				pcm_max_read_len = pcm_max_read_len_init;
				pcm_open_retry_pcm_samples = 0;
				pcm_open_retries++;
				free(tmp);
				continue;
			}

			snd_pcm_get_params(w->snd_pcm, &buffer_frames, &period_frames);
			pcm_max_read_len = period_frames * w->ba_pcm.channels * pcm_format_size;

			if (mixer_device != NULL) {
				debug("Opening ALSA mixer: name=%s elem=%s index=%u",
						mixer_device, mixer_elem_name, mixer_elem_index);
				if (alsa_mixer_open(&w->snd_mixer, &w->snd_mixer_elem,
							mixer_device, mixer_elem_name, mixer_elem_index, &tmp) != 0) {
					warn("Couldn't open ALSA mixer: %s", tmp);
					free(tmp);
				}
			}

			io_worker_mixer_volume_sync_setup(w);

			/* reset retry counters */
			pcm_open_retry_pcm_samples = 0;
			pcm_open_retries = 0;

			if (verbose >= 2) {
				info("Used configuration for %s:\n"
						"  ALSA PCM buffer time: %u us (%zu bytes)\n"
						"  ALSA PCM period time: %u us (%zu bytes)\n"
						"  PCM format: %s\n"
						"  Sampling rate: %u Hz\n"
						"  Channels: %u",
						w->addr,
						buffer_time, snd_pcm_frames_to_bytes(w->snd_pcm, buffer_frames),
						period_time, snd_pcm_frames_to_bytes(w->snd_pcm, period_frames),
						snd_pcm_format_name(pcm_format),
						w->ba_pcm.sampling,
						w->ba_pcm.channels);
			}

			if (verbose >= 3)
				alsa_pcm_dump(w->snd_pcm, stderr);

		}

		/* mark device as active and set timeout to 500ms */
		w->active = true;
		timeout = 500;

		/* Current worker was marked as active, so we can safely
		 * release the single playback mutex if it was locked. */
		if (single_playback_mutex_locked) {
			pthread_mutex_unlock(&single_playback_mutex);
			single_playback_mutex_locked = false;
		}

		ffb_seek(&buffer, read_samples);
		size_t samples = ffb_len_out(&buffer);

		if (!w->mixer_has_mute_switch && pcm_muted)
			snd_pcm_format_set_silence(pcm_format, buffer.data, samples);

		snd_pcm_sframes_t frames;

retry_alsa_write:
		frames = samples / w->ba_pcm.channels;
		if ((frames = snd_pcm_writei(w->snd_pcm, buffer.data, frames)) < 0)
			switch (-frames) {
			case EINTR:
				goto retry_alsa_write;
			case EPIPE:
				debug("ALSA playback PCM underrun");
				snd_pcm_prepare(w->snd_pcm);
				goto retry_alsa_write;
			default:
				error("ALSA playback PCM write error: %s", snd_strerror(frames));
				goto close_alsa;
			}

		/* move leftovers to the beginning and reposition tail */
		ffb_shift(&buffer, frames * w->ba_pcm.channels);

		continue;

close_alsa:
		ffb_rewind(&buffer);
		pcm_max_read_len = pcm_max_read_len_init;
		if (w->snd_pcm != NULL) {
			snd_pcm_close(w->snd_pcm);
			w->snd_pcm = NULL;
		}
		if (w->snd_mixer != NULL) {
			snd_mixer_close(w->snd_mixer);
			w->snd_mixer_elem = NULL;
			w->snd_mixer = NULL;
		}
	}

fail:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static bool pcm_hw_params_equal(
		const struct ba_pcm *ba_pcm_1,
		const struct ba_pcm *ba_pcm_2) {
	if (ba_pcm_1->format != ba_pcm_2->format)
		return false;
	if (ba_pcm_1->channels != ba_pcm_2->channels)
		return false;
	if (ba_pcm_1->sampling != ba_pcm_2->sampling)
		return false;
	return true;
}

/**
 * Stop the worker thread at workers[index]. */
static void io_worker_stop(size_t index) {

	/* Safety check for out-of-bounds read. */
	assert(index < workers_count);

	pthread_rwlock_wrlock(&workers_lock);

	pthread_cancel(workers[index].thread);
	pthread_join(workers[index].thread, NULL);

	if (index != --workers_count)
		/* Move the last worker in the array to position
		 * index, to prevent any "gaps" in the array. */
		memcpy(&workers[index], &workers[workers_count], sizeof(workers[index]));

	pthread_rwlock_unlock(&workers_lock);

}

static struct io_worker *supervise_io_worker_start(const struct ba_pcm *ba_pcm) {

	size_t i;
	for (i = 0; i < workers_count; i++)
		if (strcmp(workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
			/* If the codec has changed after the device connected, then the
			 * audio format may have changed. If it has, the worker thread
			 * needs to be restarted. */
			if (!pcm_hw_params_equal(&workers[i].ba_pcm, ba_pcm))
				io_worker_stop(i);
			else
				return &workers[i];
		}

	pthread_rwlock_wrlock(&workers_lock);

	workers_count++;
	if (workers_size < workers_count) {
		struct io_worker *tmp = workers;
		workers_size += 4;  /* coarse-grained realloc */
		if ((workers = realloc(workers, sizeof(*workers) * workers_size)) == NULL) {
			error("Couldn't (re)allocate memory for IO workers: %s", strerror(ENOMEM));
			workers = tmp;
			pthread_rwlock_unlock(&workers_lock);
			return NULL;
		}
	}

	struct io_worker *worker = &workers[workers_count - 1];
	memcpy(&worker->ba_pcm, ba_pcm, sizeof(worker->ba_pcm));
	ba2str(&worker->ba_pcm.addr, worker->addr);
	worker->ba_pcm_fd = -1;
	worker->ba_pcm_ctrl_fd = -1;
	worker->snd_pcm = NULL;
	worker->snd_mixer = NULL;
	worker->snd_mixer_elem = NULL;
	worker->mixer_has_mute_switch = false;
	worker->active = false;

	pthread_rwlock_unlock(&workers_lock);

	debug("Creating IO worker %s", worker->addr);

	if ((errno = pthread_create(&worker->thread, NULL,
					PTHREAD_FUNC(io_worker_routine), worker)) != 0) {
		error("Couldn't create IO worker %s: %s", worker->addr, strerror(errno));
		workers_count--;
		return NULL;
	}

	return worker;
}

static struct io_worker *supervise_io_worker_stop(const struct ba_pcm *ba_pcm) {

	size_t i;
	for (i = 0; i < workers_count; i++)
		if (strcmp(workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path) == 0)
			io_worker_stop(i);

	return NULL;
}

static struct io_worker *supervise_io_worker(const struct ba_pcm *ba_pcm) {

	if (ba_pcm == NULL)
		return NULL;

	if (ba_pcm->mode != BA_PCM_MODE_SOURCE)
		goto stop;

	if ((ba_profile_a2dp && !(ba_pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP)) ||
			(!ba_profile_a2dp && !(ba_pcm->transport & BA_PCM_TRANSPORT_MASK_SCO)))
		goto stop;

	/* check whether SCO has selected codec */
	if (ba_pcm->transport & BA_PCM_TRANSPORT_MASK_SCO &&
			ba_pcm->sampling == 0) {
		debug("Skipping SCO with codec not selected");
		goto stop;
	}

	if (ba_addr_any)
		goto start;

	size_t i;
	for (i = 0; i < ba_addrs_count; i++)
		if (bacmp(&ba_addrs[i], &ba_pcm->addr) == 0)
			goto start;

stop:
	return supervise_io_worker_stop(ba_pcm);
start:
	return supervise_io_worker_start(ba_pcm);
}

static DBusHandlerResult dbus_signal_handler(DBusConnection *conn, DBusMessage *message, void *data) {
	(void)conn;
	(void)data;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *path = dbus_message_get_path(message);
	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	DBusMessageIter iter;
	struct io_worker *worker;

	if (strcmp(interface, DBUS_INTERFACE_OBJECT_MANAGER) == 0) {

		if (strcmp(signal, "InterfacesAdded") == 0) {
			if (!dbus_message_iter_init(message, &iter))
				goto fail;
			struct ba_pcm pcm;
			DBusError err = DBUS_ERROR_INIT;
			if (!bluealsa_dbus_message_iter_get_pcm(&iter, &err, &pcm)) {
				error("Couldn't add new BlueALSA PCM: %s", err.message);
				dbus_error_free(&err);
				goto fail;
			}
			if (pcm.transport == BA_PCM_TRANSPORT_NONE)
				goto fail;
			if (ba_pcm_add(&pcm) == NULL) {
				error("Couldn't add new BlueALSA PCM: %s", strerror(errno));
				goto fail;
			}
			supervise_io_worker(&pcm);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		if (strcmp(signal, "InterfacesRemoved") == 0) {
			if (!dbus_message_iter_init(message, &iter) ||
					dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
				error("Couldn't remove BlueALSA PCM: %s", "Invalid signal signature");
				goto fail;
			}
			dbus_message_iter_get_basic(&iter, &path);
			struct ba_pcm *pcm;
			if ((pcm = ba_pcm_get(path)) == NULL)
				goto fail;
			supervise_io_worker_stop(pcm);
			ba_pcm_remove(path);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

	}

	if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0) {
		struct ba_pcm *pcm;
		if ((pcm = ba_pcm_get(path)) == NULL)
			goto fail;
		if (!dbus_message_iter_init(message, &iter) ||
				dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
			error("Couldn't update BlueALSA PCM: %s", "Invalid signal signature");
			goto fail;
		}
		dbus_message_iter_get_basic(&iter, &interface);
		dbus_message_iter_next(&iter);
		if (!bluealsa_dbus_message_iter_get_pcm_props(&iter, NULL, pcm))
			goto fail;
		if ((worker = supervise_io_worker(pcm)) != NULL)
			io_worker_mixer_volume_sync_snd_mixer_elem(worker, pcm);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVSvlLB:D:M:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "syslog", no_argument, NULL, 'S' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "list-devices", no_argument, NULL, 'l' },
		{ "list-pcms", no_argument, NULL, 'L' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "pcm", required_argument, NULL, 'D' },
		{ "pcm-buffer-time", required_argument, NULL, 3 },
		{ "pcm-period-time", required_argument, NULL, 4 },
		{ "volume", required_argument, NULL, '8' },
		{ "mixer-device", required_argument, NULL, 'M' },
		{ "mixer-name", required_argument, NULL, 6 },
		{ "mixer-index", required_argument, NULL, 7 },
		{ "profile-a2dp", no_argument, NULL, 1 },
		{ "profile-sco", no_argument, NULL, 2 },
		{ "single-audio", no_argument, NULL, 5 },
		{ 0, 0, 0, 0 },
	};

	bool syslog = false;
	const char *volume_type_str = "auto";

	/* Check if syslog forwarding has been enabled. This check has to be
	 * done before anything else, so we can log early stage warnings and
	 * errors. */
	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("Usage:\n"
					"  %s [OPTION]... [BT-ADDR]...\n"
					"\nOptions:\n"
					"  -h, --help\t\t\tprint this help and exit\n"
					"  -V, --version\t\t\tprint version and exit\n"
					"  -S, --syslog\t\t\tsend output to syslog\n"
					"  -v, --verbose\t\t\tmake output more verbose\n"
					"  -l, --list-devices\t\tlist available BT audio devices\n"
					"  -L, --list-pcms\t\tlist available BT audio PCMs\n"
					"  -B, --dbus=NAME\t\tBlueALSA service name suffix\n"
					"  -D, --pcm=NAME\t\tplayback PCM device to use\n"
					"  --pcm-buffer-time=INT\t\tplayback PCM buffer time\n"
					"  --pcm-period-time=INT\t\tplayback PCM period time\n"
					"  --volume=TYPE\t\t\tvolume control type [auto|mixer|none|software]\n"
					"  -M, --mixer-device=NAME\tmixer device to use\n"
					"  --mixer-name=NAME\t\tmixer element name\n"
					"  --mixer-index=NUM\t\tmixer element index\n"
					"  --profile-a2dp\t\tuse A2DP profile (default)\n"
					"  --profile-sco\t\t\tuse SCO profile\n"
					"  --single-audio\t\tsingle audio mode\n"
					"\nNote:\n"
					"If one wants to receive audio from more than one Bluetooth device, it is\n"
					"possible to specify more than one MAC address. By specifying any/empty MAC\n"
					"address (00:00:00:00:00:00), one will allow connections from any Bluetooth\n"
					"device. Without given explicit MAC address any/empty MAC is assumed.\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'S' /* --syslog */ :
			syslog = true;
			break;

		case 'v' /* --verbose */ :
			verbose++;
			break;
		}

	log_open(basename(argv[0]), syslog);
	dbus_threads_init_default();

	/* parse options */
	optind = 0; opterr = 1;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
		case 'V' /* --version */ :
		case 'S' /* --syslog */ :
		case 'v' /* --verbose */ :
			break;

		case 'l' /* --list-devices */ :
			list_bt_devices = true;
			break;
		case 'L' /* --list-pcms */ :
			list_bt_pcms = true;
			break;

		case 'B' /* --dbus=NAME */ :
			snprintf(dbus_ba_service, sizeof(dbus_ba_service), BLUEALSA_SERVICE ".%s", optarg);
			if (!dbus_validate_bus_name(dbus_ba_service, NULL)) {
				error("Invalid BlueALSA D-Bus service name: %s", dbus_ba_service);
				return EXIT_FAILURE;
			}
			break;

		case 'D' /* --pcm=NAME */ :
			pcm_device = optarg;
			break;
		case 3 /* --pcm-buffer-time=INT */ :
			pcm_buffer_time = atoi(optarg);
			break;
		case 4 /* --pcm-period-time=INT */ :
			pcm_period_time = atoi(optarg);
			break;

		case '8' /* --volume */ : {

			static const nv_entry_t values[] = {
				{ "auto", .v.ui = VOL_TYPE_AUTO },
				{ "mixer", .v.ui = VOL_TYPE_MIXER },
				{ "software", .v.ui = VOL_TYPE_SOFTWARE },
				{ "none", .v.ui = VOL_TYPE_NONE },
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid volume control type {%s}: %s",
						nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			volume_type_str = optarg;
			volume_type = entry->v.ui;
			break;
		}

		case 'M' /* --mixer-device=NAME */ :
			mixer_device = optarg;
			break;
		case 6 /* --mixer-name=NAME */ :
			mixer_elem_name = optarg;
			break;
		case 7 /* --mixer-index=NUM */ :
			mixer_elem_index = atoi(optarg);
			break;

		case 1 /* --profile-a2dp */ :
			ba_profile_a2dp = true;
			break;
		case 2 /* --profile-sco */ :
			ba_profile_a2dp = false;
			break;

		case 5 /* --single-audio */ :
			force_single_playback = true;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
		error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	if (list_bt_devices || list_bt_pcms) {

		if (!bluealsa_dbus_get_pcms(&dbus_ctx, &ba_pcms, &ba_pcms_count, &err)) {
			warn("Couldn't get BlueALSA PCM list: %s", err.message);
			return EXIT_FAILURE;
		}

		if (list_bt_pcms)
			print_bt_pcm_list();

		if (list_bt_devices)
			print_bt_device_list();

		return EXIT_SUCCESS;
	}

	if (optind == argc)
		ba_addr_any = true;
	else if (parse_bt_addresses(&argv[optind], argc - optind) == -1) {
		error("Couldn't parse BT addresses: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	if (volume_type == VOL_TYPE_NONE || volume_type == VOL_TYPE_SOFTWARE)
		mixer_device = NULL;

	if (verbose >= 1) {

		char *ba_str = malloc(19 * ba_addrs_count + 1);
		char *tmp = ba_str;
		size_t i;

		for (i = 0; i < ba_addrs_count; i++, tmp += 19)
			ba2str(&ba_addrs[i], stpcpy(tmp, ", "));

		const char *mixer_device_str = "(not used)";
		char mixer_element_str[128] = "(not used)";
		if (mixer_device != NULL) {
			mixer_device_str = mixer_device;
			snprintf(mixer_element_str, sizeof(mixer_element_str), "'%s',%u",
					mixer_elem_name, mixer_elem_index);
		}

		info("Selected configuration:\n"
				"  BlueALSA service: %s\n"
				"  ALSA PCM device: %s\n"
				"  ALSA PCM buffer time: %u us\n"
				"  ALSA PCM period time: %u us\n"
				"  Volume control type: %s\n"
				"  ALSA mixer device: %s\n"
				"  ALSA mixer element: %s\n"
				"  Bluetooth device(s): %s\n"
				"  Profile: %s",
				dbus_ba_service,
				pcm_device, pcm_buffer_time, pcm_period_time,
				volume_type_str,
				mixer_device_str,
				mixer_element_str,
				ba_addr_any ? "ANY" : &ba_str[2],
				ba_profile_a2dp ? "A2DP" : "SCO");

		free(ba_str);
	}

	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded",
			"path_namespace='/org/bluealsa'");
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved",
			"path_namespace='/org/bluealsa'");
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged",
			"arg0='"BLUEALSA_INTERFACE_PCM"'");

	if (!dbus_connection_add_filter(dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
		error("Couldn't add D-Bus filter: %s", err.message);
		return EXIT_FAILURE;
	}

	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &ba_pcms, &ba_pcms_count, &err))
		warn("Couldn't get BlueALSA PCM list: %s", err.message);

	for (size_t i = 0; i < ba_pcms_count; i++)
		supervise_io_worker(&ba_pcms[i]);

	struct sigaction sigact = { .sa_handler = main_loop_stop };
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	debug("Starting main loop");
	while (main_loop_on) {

		struct pollfd pfds[10];
		nfds_t pfds_len = ARRAYSIZE(pfds);

		if (!bluealsa_dbus_connection_poll_fds(&dbus_ctx, pfds, &pfds_len)) {
			error("Couldn't get D-Bus connection file descriptors");
			return EXIT_FAILURE;
		}

		if (poll(pfds, pfds_len, -1) == -1 &&
				errno == EINTR)
			continue;

		if (bluealsa_dbus_connection_poll_dispatch(&dbus_ctx, pfds, pfds_len))
			while (dbus_connection_dispatch(dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
				continue;

	}

	for (size_t i = 0; i < workers_count; i++)
		io_worker_stop(i);
	free(workers);

	bluealsa_dbus_connection_ctx_free(&dbus_ctx);
	return EXIT_SUCCESS;
}
