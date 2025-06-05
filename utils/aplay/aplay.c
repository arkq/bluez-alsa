/*
 * BlueALSA - aplay.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/nv.h"
#include "alsa-mixer.h"
#include "alsa-pcm.h"
#include "dbus.h"
#include "delay-report.h"
#if WITH_LIBSAMPLERATE
# include "resampler.h"
#endif

/* Many devices cannot synchronize A/V with very high audio latency. To keep
 * the overall latency below 400ms we choose default ALSA parameters such that
 * the ALSA latency for A2DP is below 200ms. For SCO we choose to prioritize
 * much lower latency over audio quality. */
#define DEFAULT_PERIOD_TIME_A2DP 50000
#define DEFAULT_PERIOD_TIME_SCO 20000
#define DEFAULT_PERIODS 4

enum volume_type {
	VOL_TYPE_AUTO,
	VOL_TYPE_MIXER,
	VOL_TYPE_SOFTWARE,
	VOL_TYPE_NONE,
};

struct io_worker {
	pthread_t thread;
	bool thread_started;
	/* thread-safety for worker data access */
	pthread_mutex_t mutex;
	/* used BlueALSA PCM device */
	struct ba_pcm ba_pcm;
	/* file descriptor of PCM FIFO */
	int ba_pcm_fd;
	/* file descriptor of PCM control */
	int ba_pcm_ctrl_fd;
	/* opened playback PCM device */
	struct alsa_pcm	alsa_pcm;
	/* mixer for volume control */
	struct alsa_mixer alsa_mixer;
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
static unsigned int pcm_buffer_time = 0;
static unsigned int pcm_period_time = 0;
#if WITH_LIBSAMPLERATE
static enum resampler_converter_type resampler_method = RESAMPLER_CONV_NONE;
#endif

/* local PCM muted state for software mute */
static bool pcm_muted = false;

static struct ba_dbus_ctx dbus_ctx;
static char dbus_ba_service[32] = BLUEALSA_SERVICE;

static struct ba_pcm *ba_pcms = NULL;
static size_t ba_pcms_count = 0;

static pthread_mutex_t single_playback_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool force_single_playback = false;

static pthread_rwlock_t workers_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct io_worker *workers[16] = { NULL };
static size_t workers_size = ARRAYSIZE(workers);

static int main_loop_quit_event_fd = -1;
static void main_loop_stop(int sig) {
	eventfd_write(main_loop_quit_event_fd, sig);
}

static int parse_bt_addresses(char *argv[], size_t count) {

	ba_addrs_count = count;
	if ((ba_addrs = malloc(sizeof(*ba_addrs) * ba_addrs_count)) == NULL)
		return -1;

	for (size_t i = 0; i < ba_addrs_count; i++) {
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
	size_t ii;

	for (size_t i = 0; i < ARRAYSIZE(section); i++) {
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
					pcm->rate);

		}
	}

}

static void print_bt_pcm_list(void) {

	DBusError err = DBUS_ERROR_INIT;
	struct bluez_device dev = { 0 };
	const char *tmp = "";

	for (size_t i = 0; i < ba_pcms_count; i++) {
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
				pcm->rate);

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

	pthread_rwlock_rdlock(&workers_lock);

	struct io_worker *w = NULL;
	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] != NULL && workers[i]->active) {
			w = workers[i];
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

	unsigned int volume;
	/* If mixer element does not support playback switch,
	 * use our global muted state as a default value. */
	bool muted = pcm_muted;

	const int vmax = BA_PCM_VOLUME_MAX(ba_pcm);
	if (alsa_mixer_get_volume(&worker->alsa_mixer, vmax, &volume, &muted) != 0)
		return -1;

	for (size_t i = 0; i < ba_pcm->channels; i++) {
		ba_pcm->volume[i].muted = muted;
		ba_pcm->volume[i].volume = volume;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_pcm_update(&dbus_ctx, ba_pcm, BLUEALSA_PCM_VOLUME, &err)) {
		error("Couldn't update BlueALSA source PCM: %s", err.message);
		dbus_error_free(&err);
		return -1;
	}

	return 0;
}

/**
 * Update ALSA mixer element according to BlueALSA PCM volume. */
static int io_worker_mixer_volume_sync_alsa_mixer(
		struct io_worker *worker,
		struct ba_pcm *ba_pcm) {

	/* Skip update in case of software volume. */
	if (ba_pcm->soft_volume)
		return 0;

	int ret = 0;

	/* This function is called by the D-Bus signal handler, so we have to
	 * make sure that we will not have any interference from the IO thread
	 * trying to modify ALSA mixer at the same time. */
	pthread_mutex_lock(&worker->mutex);

	if (!alsa_mixer_is_open(&worker->alsa_mixer))
		goto final;

	/* User can connect BlueALSA PCM to mono, stereo or multi-channel output.
	 * For mono input (audio from BlueALSA PCM), case case is simple: we are
	 * changing all output channels at once. However, for stereo input it is
	 * not possible to know how to control left/right volume unless there is
	 * some kind of channel mapping. In order to simplify things, we will set
	 * all channels to the average left-right volume. */

	unsigned int volume_sum = 0, muted = 0;
	for (size_t i = 0; i < ba_pcm->channels; i++) {
		volume_sum += ba_pcm->volume[i].volume;
		muted |= ba_pcm->volume[i].muted;
	}

	/* keep local muted state up to date */
	pcm_muted = muted;

	const unsigned int vmax = BA_PCM_VOLUME_MAX(ba_pcm);
	const unsigned int volume = volume_sum / ba_pcm->channels;
	ret = alsa_mixer_set_volume(&worker->alsa_mixer, vmax, volume, muted);

final:
	pthread_mutex_unlock(&worker->mutex);
	return ret;
}

static void io_worker_mixer_event_callback(void *data) {
	struct io_worker *worker = data;
	io_worker_mixer_volume_sync_ba_pcm(worker, &worker->ba_pcm);
}

static snd_pcm_uframes_t io_worker_playback_delay(
		const struct io_worker *w,
#if WITH_LIBSAMPLERATE
		const struct resampler *resampler,
		const ffb_t *resampler_buffer,
#endif
		const ffb_t *read_buffer) {

	snd_pcm_uframes_t delay = 0;
	double rate_ratio = 1.0;

	unsigned int ba_pcm_buffered = 0;
	/* Get the delay due to BlueALSA PCM FIFO buffering. */
	ioctl(w->ba_pcm_fd, FIONREAD, &ba_pcm_buffered);
	delay += ba_pcm_buffered / read_buffer->size / w->ba_pcm.channels;

	/* Get the delay accumulated in the read buffer. */
	delay += ffb_len_out(read_buffer) / w->ba_pcm.channels;

#if WITH_LIBSAMPLERATE
	if (resampler != NULL) {
		rate_ratio = resampler_current_rate_ratio(resampler);
		/* Get the delay accumulated in the resampler. */
		delay += ffb_len_out(resampler_buffer) / w->ba_pcm.channels / rate_ratio;
	}
#endif

	/* Get the ALSA device delay. In case when resampler is used, convert
	 * ALSA device sample rate to the BlueALSA PCM sample rate. */
	delay += w->alsa_pcm.delay / rate_ratio;

	return delay;
}

static void io_worker_routine_exit(struct io_worker *w) {

	pthread_mutex_lock(&w->mutex);

	if (w->ba_pcm_fd != -1) {
		close(w->ba_pcm_fd);
		w->ba_pcm_fd = -1;
	}
	if (w->ba_pcm_ctrl_fd != -1) {
		close(w->ba_pcm_ctrl_fd);
		w->ba_pcm_ctrl_fd = -1;
	}

	alsa_pcm_close(&w->alsa_pcm);
	alsa_mixer_close(&w->alsa_mixer);

	debug("Exiting IO worker %s", w->addr);
	pthread_mutex_unlock(&w->mutex);

}

static void *io_worker_routine(struct io_worker *w) {

	const snd_pcm_format_t pcm_format = bluealsa_get_snd_pcm_format(&w->ba_pcm);
	const ssize_t pcm_format_size = snd_pcm_format_size(pcm_format, 1);
	const size_t pcm_1s_samples = w->ba_pcm.rate * w->ba_pcm.channels;
	/* Buffer for audio frames read from the BlueALSA server. */
	ffb_t read_buffer = { 0 };
	/* Buffer from which audio frames are written to the ALSA PCM. If the
	 * resampler is used then this is the resampler output buffer. Otherwise
	 * it is the same as the read buffer. */
	ffb_t *write_buffer = &read_buffer;
	/* Preferred format for the ALSA PCM. If not using the resampler then this
	 * is the format of the incoming BlueALSA stream. */
	snd_pcm_format_t format_1 = pcm_format;
	/* Alternative format that can be generated internally by the resampler.
	 * This is only used if the resampler is enabled. */
	snd_pcm_format_t format_2 = SND_PCM_FORMAT_UNKNOWN;

#if WITH_LIBSAMPLERATE
	/* The resampler requires the native endian format for the input data. */
	const snd_pcm_format_t resampler_pcm_format = resampler_native_endian_format(pcm_format);
	struct resampler resampler = { 0 };
	ffb_t resampled_buffer = { 0 };
	/* For detecting when the ALSA device has auto-started after reaching its
	 * start threshold. */
	bool alsa_pcm_started = false;
	bool use_resampler = false;
#endif

	int pcm_flags = 0;

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	pthread_cleanup_push(PTHREAD_CLEANUP(io_worker_routine_exit), w);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &read_buffer);
#if WITH_LIBSAMPLERATE
	pthread_cleanup_push(PTHREAD_CLEANUP(resampler_free), &resampler);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &resampled_buffer);
#endif

	/* Create a buffer big enough to hold enough PCM data for three periods.
	 * This will be later be revised if necessary to match the actual ALSA
	 * start threshold when the ALSA PCM is opened. */
	const size_t nmemb = ((size_t)pcm_period_time * 3 / 1000) * (pcm_1s_samples / 1000);
	if (ffb_init(&read_buffer, nmemb, pcm_format_size) == -1) {
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
			if (!ba_dbus_pcm_update(&dbus_ctx, &w->ba_pcm, BLUEALSA_PCM_SOFT_VOLUME, &err)) {
				error("Couldn't set BlueALSA source PCM volume mode: %s", err.message);
				dbus_error_free(&err);
				goto fail;
			}
		}
	}

	debug("Opening BlueALSA source PCM: %s", w->ba_pcm.pcm_path);
	if (!ba_dbus_pcm_open(&dbus_ctx, w->ba_pcm.pcm_path,
				&w->ba_pcm_fd, &w->ba_pcm_ctrl_fd, &err)) {
		error("Couldn't open BlueALSA source PCM: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

#if WITH_LIBSAMPLERATE
	if (resampler_method != RESAMPLER_CONV_NONE) {
		if (!resampler_is_input_format_supported(pcm_format))
			warn("Resampler not enabled: Unsupported input format: %s",
					snd_pcm_format_name(pcm_format));
		else {
			use_resampler = true;
			/* Disable alsa-lib resampling because our internal resampler can
			 * convert to a rate natively supported by the sound card. Initially
			 * also disable alsa-lib format conversion to try to use a format
			 * generated by the resampler. If that fails, then enable alsa-lib
			 * format conversion and have the resampler output its preferred
			 * FLOAT format. In this way the number of format conversions in the
			 * processing chain is minimized. */
			pcm_flags = SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_FORMAT;
			/* The resampler uses FLOAT internally, so prefer that if the ALSA
			 * device supports it, to avoid the need to convert. Otherwise, use
			 * the resampler native endian integer format - input format. */
			format_1 = resampler_preferred_output_format();
			format_2 = resampler_pcm_format;
		}
	}
#endif

	/* Track the lock state of the single playback mutex within this thread. */
	bool single_playback_mutex_locked = false;

	/* Intervals in seconds between consecutive PCM open retry attempts. */
	const unsigned int pcm_open_retry_intervals[] = { 1, 1, 2, 3, 5 };
	size_t pcm_open_retry_pcm_samples = 0;
	size_t pcm_open_retries = 0;

	struct delay_report dr;
	delay_report_init(&dr, &dbus_ctx, &w->ba_pcm);

	size_t pause_retry_pcm_samples = pcm_1s_samples;
	size_t pause_retries = 0;

	int timeout = -1;

	debug("Starting IO loop");
	for (;;) {

		if (single_playback_mutex_locked) {
			pthread_mutex_unlock(&single_playback_mutex);
			single_playback_mutex_locked = false;
		}

		struct pollfd fds[16] = {
			{ main_loop_quit_event_fd, POLLIN, 0 },
			{ w->ba_pcm_fd, POLLIN, 0 }};
		nfds_t nfds = 2;

		if (alsa_mixer_is_open(&w->alsa_mixer)) {
			nfds += alsa_mixer_poll_descriptors_count(&w->alsa_mixer);
			if (nfds <= ARRAYSIZE(fds))
				alsa_mixer_poll_descriptors(&w->alsa_mixer, fds + 2, nfds - 2);
			else {
				error("Poll FD array size exceeded: %zu > %zu", nfds, ARRAYSIZE(fds));
				goto fail;
			}
		}

		/* Reading from the FIFO won't block unless there is an open connection
		 * on the writing side. However, the server does not open PCM FIFO until
		 * a transport is created. With the A2DP, the transport is created when
		 * some clients (BT device) requests audio transfer. */

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		int poll_rv = poll(fds, nfds, timeout);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		pthread_mutex_lock(&w->mutex);
		/* Check the PCM running status on every iteration. */
		bool ba_pcm_running = w->ba_pcm.running;
		pthread_mutex_unlock(&w->mutex);

		if (poll_rv == -1) {
			if (errno == EINTR)
				continue;
			error("IO loop poll error: %s", strerror(errno));
			goto fail;
		}

		if (poll_rv == 0 &&
				ba_pcm_running &&
				w->active &&
				ffb_blen_out(&read_buffer) == 0 &&
				!alsa_pcm_is_running(&w->alsa_pcm)) {
			/* The BT device is in the running state, but is not sending audio
			 * frames. As there is no work for the ALSA device to do we simply
			 * wait for more audio to arrive from the server. */
			timeout = -1;
			continue;
		}

		if (fds[0].revents & POLLIN)
			break;

		if (alsa_mixer_is_open(&w->alsa_mixer))
			alsa_mixer_handle_events(&w->alsa_mixer);

		size_t read_samples = 0;
		if (fds[1].revents & POLLIN) {

			/* If the read buffer is full then we have an overrun. We must
			 * discard audio frames in order to continue reading fresh data
			 * from the server. */
			if (ffb_blen_in(&read_buffer) == 0) {
				unsigned int buffered = 0;
				ioctl(w->ba_pcm_fd, FIONREAD, &buffered);
				const size_t discard_bytes = MIN(buffered, ffb_blen_out(&read_buffer));
				const size_t discard_samples = discard_bytes / pcm_format_size;
				ffb_shift(&read_buffer, discard_samples);
				if (alsa_pcm_is_open(&w->alsa_pcm))
					warn("Dropping PCM frames: %zu", discard_samples / w->ba_pcm.channels);
			}

			ssize_t ret;
			if ((ret = read(w->ba_pcm_fd, read_buffer.tail, ffb_blen_in(&read_buffer))) == -1) {
				if (errno == EINTR)
					continue;
				error("BlueALSA source PCM read error: %s", strerror(errno));
				goto fail;
			}

			read_samples = ret / pcm_format_size;
			if (ret % pcm_format_size != 0)
				warn("Invalid read from BlueALSA source PCM: %zd %% %zd != 0", ret, pcm_format_size);

#if WITH_LIBSAMPLERATE
			if (use_resampler)
				resampler_convert_to_native_endian_format(read_buffer.tail, read_samples, pcm_format);
#endif
			ffb_seek(&read_buffer, read_samples);

		}
		else if (fds[1].revents & POLLHUP) {
			/* Source PCM FIFO has been terminated on the writing side. */
			debug("BlueALSA source PCM disconnected: %s", w->ba_pcm.pcm_path);
			ba_pcm_running = false;
			break;
		}
		else if (fds[1].revents) {
			error("Unexpected BlueALSA source PCM poll event: %#x", fds[1].revents);
		}

		/* If current worker is not active and the single playback mode was
		 * enabled, we have to check if there is any other active worker. */
		if (!w->active) {

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

		if (!alsa_pcm_is_open(&w->alsa_pcm)) {

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

			debug("Opening ALSA playback PCM: name=%s channels=%u rate%s=%u",
					pcm_device, w->ba_pcm.channels,
					pcm_flags & SND_PCM_NO_AUTO_RESAMPLE ? "~" : "",
					w->ba_pcm.rate);

			char *tmp = NULL;
			int res = alsa_pcm_open(&w->alsa_pcm, pcm_device, format_1, format_2,
						w->ba_pcm.channels, w->ba_pcm.rate, pcm_buffer_time,
						pcm_period_time, pcm_flags, &tmp);
			switch (res) {
			case 0:
				break;
			case -EINVAL:
#if WITH_LIBSAMPLERATE
				/* If the PCM failed to open because the sound card does not
				 * natively support either the float or integer formats of the
				 * resampler, then try again but this time with alsa-lib format
				 * conversion enabled. */
				if (use_resampler && w->alsa_pcm.format == SND_PCM_FORMAT_UNKNOWN) {
					free(tmp);
					if (alsa_pcm_open(&w->alsa_pcm, pcm_device, format_1,
								format_2, w->ba_pcm.channels, w->ba_pcm.rate,
								pcm_buffer_time, pcm_period_time,
								pcm_flags & ~SND_PCM_NO_AUTO_FORMAT, &tmp) == 0) {
						break;
					}
				}
#endif
				/* fall-through */
			default:
				warn("Couldn't open ALSA playback PCM: %s", tmp);
				pcm_open_retry_pcm_samples = 0;
				pcm_open_retries++;
				free(tmp);
				continue;
			}

			/* Resize the read buffer to ensure it is not less than the
			 * ALSA start threshold. This is to ensure that the PCM re-starts
			 * quickly after an overrun. */
			if (w->alsa_pcm.start_threshold > read_buffer.nmemb / w->ba_pcm.channels)
				ffb_init(&read_buffer, w->alsa_pcm.start_threshold * w->ba_pcm.channels, read_buffer.size);

#if WITH_LIBSAMPLERATE
			if (use_resampler) {

				/* Initialize the resampler which will try to keep the playback delay
				 * within configured limits. The lower and upper limits are set to
				 * the ALSA start threshold and the start threshold plus the period
				 * size respectively. The resampler will adapt the rate to keep the
				 * delay within these limits. */
				if ((res = resampler_init(
								&resampler,
								resampler_method,
								w->ba_pcm.channels,
								resampler_pcm_format,
								w->ba_pcm.rate,
								w->alsa_pcm.format,
								w->alsa_pcm.rate,
								w->alsa_pcm.start_threshold,
								w->alsa_pcm.start_threshold + w->alsa_pcm.period_frames)) == -1) {
					error("Couldn't initialize resampler: %s", strerror(errno));
					goto fail;
				}

#if DEBUG
				if (verbose >= 4)
					debug("PCM sample rate conversion: %u Hz -> %#.2f Hz", w->ba_pcm.rate,
							w->ba_pcm.rate * resampler_current_rate_ratio(&resampler));
#endif

				/* The resampler output buffer is sized to accommodate the
				 * result of resampling a full read_buffer, plus a little extra
				 * to allow for positive adaptive resampling adjustment. */
				size_t buffer_size = read_buffer.nmemb * w->alsa_pcm.rate / w->ba_pcm.rate;
				buffer_size = (buffer_size * 110) / 100;
				ffb_init(&resampled_buffer, buffer_size, snd_pcm_format_size(w->alsa_pcm.format, 1));
				write_buffer = &resampled_buffer;

			}
#endif

			/* Skip mixer setup in case of software volume. */
			if (mixer_device != NULL && !w->ba_pcm.soft_volume) {
				pthread_mutex_lock(&w->mutex);
				debug("Opening ALSA mixer: name=%s elem=%s index=%u",
						mixer_device, mixer_elem_name, mixer_elem_index);
				if (alsa_mixer_open(&w->alsa_mixer, mixer_device,
							mixer_elem_name, mixer_elem_index, &tmp) == 0)
					io_worker_mixer_volume_sync_ba_pcm(w, &w->ba_pcm);
				else {
					warn("Couldn't open ALSA mixer: %s", tmp);
					free(tmp);
				}
				pthread_mutex_unlock(&w->mutex);
			}

			/* Reset retry counters. */
			pcm_open_retry_pcm_samples = 0;
			pcm_open_retries = 0;

			/* Reset moving delay window buffer. */
			delay_report_reset(&dr);

			if (verbose >= 2) {
				info("Used configuration for %s:\n"
						"  BlueALSA PCM format: %s\n"
						"  BlueALSA PCM sample rate: %u Hz\n"
						"  BlueALSA PCM channels: %u\n"
						"  ALSA PCM buffer time: %u us (%zu bytes)\n"
						"  ALSA PCM period time: %u us (%zu bytes)\n"
						"  ALSA PCM format: %s\n"
						"  ALSA PCM sample rate: %u Hz\n"
						"  ALSA PCM channels: %u\n"
						"  ALSA mixer volume mapping: %s",
						w->addr,
						snd_pcm_format_name(pcm_format),
						w->ba_pcm.rate,
						w->ba_pcm.channels,
						w->alsa_pcm.buffer_time, alsa_pcm_frames_to_bytes(&w->alsa_pcm, w->alsa_pcm.buffer_frames),
						w->alsa_pcm.period_time, alsa_pcm_frames_to_bytes(&w->alsa_pcm, w->alsa_pcm.period_frames),
						snd_pcm_format_name(w->alsa_pcm.format),
						w->alsa_pcm.rate,
						w->alsa_pcm.channels,
						w->alsa_mixer.mixer ? (w->alsa_mixer.has_db_scale ? "dB scale" : "linear") : "none");
			}

			if (verbose >= 3)
				alsa_pcm_dump(&w->alsa_pcm, stderr);

		}

		/* Mark device as active. */
		w->active = true;

		/* Current worker was marked as active, so we can safely
		 * release the single playback mutex if it was locked. */
		if (single_playback_mutex_locked) {
			pthread_mutex_unlock(&single_playback_mutex);
			single_playback_mutex_locked = false;
		}

		if (!w->alsa_mixer.has_mute_switch && pcm_muted) {
			snd_pcm_format_t format = w->alsa_pcm.format;
#if WITH_LIBSAMPLERATE
			if (use_resampler)
				format = resampler_pcm_format;
#endif
			snd_pcm_format_set_silence(format, read_buffer.data, ffb_len_out(&read_buffer));
		}

#if WITH_LIBSAMPLERATE
		if (use_resampler &&
				resampler_process(&resampler, &read_buffer, write_buffer) != 0)
			goto close_alsa;
#endif

		if (alsa_pcm_write(&w->alsa_pcm, write_buffer, !ba_pcm_running) < 0)
			goto close_alsa;

		if (!ba_pcm_running)
			goto device_inactive;

		/* Set the poll() timeout such that this thread is always woken before
		 * an ALSA underrun can occur. */
		if (alsa_pcm_is_running(&w->alsa_pcm)) {
			timeout = 1000 * w->alsa_pcm.hw_avail / w->alsa_pcm.rate;
			/* poll() timeouts may be late because of the kernel scheduler and
			 * workload, and there may be additional processing delays before
			 * we can write to the ALSA PCM again. So we allow for this by setting
			 * the timeout value 5ms before the underrun deadline. */
			if ((timeout -= 5) < 0)
				timeout = 0;
		}

		const snd_pcm_uframes_t delay_frames = io_worker_playback_delay(w,
#if WITH_LIBSAMPLERATE
				use_resampler ? &resampler : NULL, &resampled_buffer,
#endif
				&read_buffer);
		if (!delay_report_update(&dr, delay_frames, &err)) {
			error("Couldn't update BlueALSA PCM client delay: %s", err.message);
			dbus_error_free(&err);
			goto fail;
		}

#if WITH_LIBSAMPLERATE
		if (use_resampler) {
			bool rate_changed = false;
			if (w->alsa_pcm.underrun) {
				debug("Resetting resampler");
				resampler_reset(&resampler);
				rate_changed = true;
			}
			if (!alsa_pcm_is_running(&w->alsa_pcm))
				alsa_pcm_started = false;
			else {
				if (!alsa_pcm_started) {
					/* The ALSA start threshold has been reached, so reset the
					 * resampler to initialize adaptive resampling. */
					resampler_reset(&resampler);
					alsa_pcm_started = true;
				}
				rate_changed = resampler_update_rate_ratio(&resampler,
						read_samples / w->ba_pcm.channels, dr.avg_value);
			}

			if (verbose >= 4 && rate_changed)
				debug("PCM sample rate conversion: %u Hz -> %#.2f Hz", w->ba_pcm.rate,
						w->ba_pcm.rate * resampler_current_rate_ratio(&resampler));
		}
#endif

		continue;

device_inactive:
		debug("BT device marked as inactive: %s", w->addr);
		pause_retry_pcm_samples = pcm_1s_samples;
		pause_retries = 0;
		timeout = -1;

close_alsa:
		ffb_rewind(&read_buffer);
#if WITH_LIBSAMPLERATE
		if (use_resampler) {
			ffb_rewind(&resampled_buffer);
			resampler_free(&resampler);
		}
#endif
		pthread_mutex_lock(&w->mutex);
		alsa_pcm_close(&w->alsa_pcm);
		alsa_mixer_close(&w->alsa_mixer);
		pthread_mutex_unlock(&w->mutex);
		w->active = !force_single_playback;
	}

fail:
#if WITH_LIBSAMPLERATE
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
#endif
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
	if (ba_pcm_1->rate != ba_pcm_2->rate)
		return false;
	return true;
}

static void io_worker_stop(struct io_worker *w) {
	if (w->thread_started) {
		pthread_cancel(w->thread);
		pthread_join(w->thread, NULL);
		w->thread_started = false;
	}
}

/**
 * Stop the IO worker thread and free its resources. */
static void io_worker_destroy(struct io_worker *w) {
	io_worker_stop(w);
	pthread_mutex_destroy(&w->mutex);
	free(w);
}

static struct io_worker *supervise_io_worker_start(const struct ba_pcm *ba_pcm) {

	struct io_worker *worker = NULL;
	char addr[sizeof(worker->addr)];
	ssize_t worker_slot = -1;

	for (size_t i = 0; i < workers_size; i++) {
		if (workers[i] == NULL) {
			if (worker_slot == -1)
				worker_slot = i;
		}
		else if (strcmp(workers[i]->ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
			/* If the codec has changed after the device connected, then the
			 * audio format may have changed. If it has, the worker thread
			 * needs to be restarted. Otherwise, update the running state. */
			if (!pcm_hw_params_equal(&workers[i]->ba_pcm, ba_pcm)) {
				io_worker_stop(workers[i]);
				worker = workers[i];
				worker_slot = i;
				break;
			}
			else {
				pthread_mutex_lock(&workers[i]->mutex);
				workers[i]->ba_pcm.running = ba_pcm->running;
				pthread_mutex_unlock(&workers[i]->mutex);
				return workers[i];
			}
		}
	}

	if (worker == NULL) {

		/* Human-readable BT address for early error reporting. */
		ba2str(&ba_pcm->addr, addr);

		if (worker_slot == -1) {
			error("Couldn't start IO worker %s: %s", addr, "No empty slots");
			return NULL;
		}

		if ((worker = malloc(sizeof(struct io_worker))) == NULL) {
			error("Couldn't start IO worker %s: %s", addr, strerror(errno));
			return NULL;
		}

		worker->thread_started = false;
		pthread_mutex_init(&worker->mutex, NULL);
		strcpy(worker->addr, addr);
		worker->ba_pcm_fd = -1;
		worker->ba_pcm_ctrl_fd = -1;

	}

	/* Synchronize access to the worker data, which we are about
	 * to modify, with other IO worker threads. */
	pthread_rwlock_wrlock(&workers_lock);

	memcpy(&worker->ba_pcm, ba_pcm, sizeof(worker->ba_pcm));
	alsa_pcm_init(&worker->alsa_pcm);
	alsa_mixer_init(&worker->alsa_mixer, io_worker_mixer_event_callback, worker);
	worker->active = !force_single_playback;

	debug("Starting IO worker %s", worker->addr);
	if ((errno = pthread_create(&worker->thread, NULL,
					PTHREAD_FUNC(io_worker_routine), worker)) == 0)
		worker->thread_started = true;
	else {
		error("Couldn't start IO worker %s: %s", worker->addr, strerror(errno));
		io_worker_destroy(worker);
		worker = NULL;
	}

	workers[worker_slot] = worker;

	pthread_rwlock_unlock(&workers_lock);

	return worker;
}

static struct io_worker *supervise_io_worker_stop(const struct ba_pcm *ba_pcm) {

	pthread_rwlock_wrlock(&workers_lock);

	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] && strcmp(workers[i]->ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
			io_worker_destroy(workers[i]);
			workers[i] = NULL;
			break;
		}

	pthread_rwlock_unlock(&workers_lock);
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
			ba_pcm->rate == 0) {
		debug("Skipping SCO with codec not selected");
		goto stop;
	}

	if (ba_addr_any)
		goto start;

	for (size_t i = 0; i < ba_addrs_count; i++)
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
			if (!dbus_message_iter_get_ba_pcm(&iter, &err, &pcm)) {
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
		if (!dbus_message_iter_get_ba_pcm_props(&iter, NULL, pcm))
			goto fail;
		if ((worker = supervise_io_worker(pcm)) != NULL)
			io_worker_mixer_volume_sync_alsa_mixer(worker, pcm);
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
		{ "loglevel", required_argument, NULL, 9 },
		{ "verbose", no_argument, NULL, 'v' },
		{ "list-devices", no_argument, NULL, 'l' },
		{ "list-pcms", no_argument, NULL, 'L' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "pcm", required_argument, NULL, 'D' },
		{ "pcm-buffer-time", required_argument, NULL, 3 },
		{ "pcm-period-time", required_argument, NULL, 4 },
		{ "volume", required_argument, NULL, '8' },
		{ "mixer-device", required_argument, NULL, 'M' },
		{ "mixer-control", required_argument, NULL, 6 },
		{ "mixer-index", required_argument, NULL, 7 },
		{ "profile-a2dp", no_argument, NULL, 1 },
		{ "profile-sco", no_argument, NULL, 2 },
#if WITH_LIBSAMPLERATE
		{ "resampler", required_argument, NULL, 10},
#endif
		{ "single-audio", no_argument, NULL, 5 },
		{ 0, 0, 0, 0 },
	};

	bool syslog = false;
	const char *volume_type_str = "auto";
#if WITH_LIBSAMPLERATE
	const char *resampler_method_str = "none";
#endif

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
					"  --loglevel=LEVEL\t\tminimum message priority\n"
					"  -v, --verbose\t\t\tmake output more verbose\n"
					"  -l, --list-devices\t\tlist available BT audio devices\n"
					"  -L, --list-pcms\t\tlist available BT audio PCMs\n"
					"  -B, --dbus=NAME\t\tBlueALSA service name suffix\n"
					"  -D, --pcm=NAME\t\tplayback PCM device to use\n"
					"  --pcm-buffer-time=INT\t\tplayback PCM buffer time\n"
					"  --pcm-period-time=INT\t\tplayback PCM period time\n"
					"  --volume=TYPE\t\t\tvolume control type [auto|mixer|none|software]\n"
					"  -M, --mixer-device=NAME\tmixer device to use\n"
					"  --mixer-control=NAME\t\tmixer control name\n"
					"  --mixer-index=NUM\t\tmixer element index\n"
					"  --profile-a2dp\t\tuse A2DP profile (default)\n"
					"  --profile-sco\t\t\tuse SCO profile\n"
#if WITH_LIBSAMPLERATE
					"  --resampler=METHOD\t\tresample conversion method\n"
#endif
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

		case 9 /* --loglevel=LEVEL */ : {

			static const nv_entry_t values[] = {
				{ "error", .v.ui = LOG_ERR },
				{ "warning", .v.ui = LOG_WARNING },
				{ "info", .v.ui = LOG_INFO },
#if DEBUG
				{ "debug", .v.ui = LOG_DEBUG },
#endif
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid loglevel {%s}: %s", nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			log_set_min_priority(entry->v.ui);
			break;
		}

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
		case 6 /* --mixer-control=NAME */ :
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

#if WITH_LIBSAMPLERATE
		case 10 /* --resampler */ : {

			static const nv_entry_t values[] = {
				{ "sinc-best", .v.ui = RESAMPLER_CONV_SINC_BEST_QUALITY },
				{ "sinc-medium", .v.ui = RESAMPLER_CONV_SINC_MEDIUM_QUALITY },
				{ "sinc-fastest", .v.ui = RESAMPLER_CONV_SINC_FASTEST },
				{ "linear", .v.ui = RESAMPLER_CONV_LINEAR },
				{ "zero-order-hold", .v.ui = RESAMPLER_CONV_ZERO_ORDER_HOLD },
				{ "none", .v.ui = RESAMPLER_CONV_NONE },
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid resampler method {%s}: %s",
						nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			resampler_method_str = optarg;
			resampler_method = entry->v.ui;
			break;
		}
#endif

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if ((main_loop_quit_event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
		error("Couldn't create quit event: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
		error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	if (list_bt_devices || list_bt_pcms) {

		if (!ba_dbus_pcm_get_all(&dbus_ctx, &ba_pcms, &ba_pcms_count, &err)) {
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

	if (pcm_buffer_time == 0) {
		if (pcm_period_time == 0)
			pcm_period_time = ba_profile_a2dp ?
				DEFAULT_PERIOD_TIME_A2DP : DEFAULT_PERIOD_TIME_SCO;
		pcm_buffer_time = pcm_period_time * DEFAULT_PERIODS;
	}
	else if (pcm_period_time == 0) {
		pcm_period_time = pcm_buffer_time / DEFAULT_PERIODS;
	}

	if (verbose >= 1) {

		char *ba_str = malloc(19 * ba_addrs_count + 1);
		char *tmp = ba_str;

		for (size_t i = 0; i < ba_addrs_count; i++, tmp += 19)
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
				"  ALSA mixer device: %s\n"
				"  ALSA mixer element: %s\n"
#if WITH_LIBSAMPLERATE
				"  Resampler method: %s\n"
#endif
				"  Volume control type: %s\n"
				"  Bluetooth device(s): %s\n"
				"  Profile: %s",
				dbus_ba_service,
				pcm_device, pcm_buffer_time, pcm_period_time,
				mixer_device_str,
				mixer_element_str,
#if WITH_LIBSAMPLERATE
				resampler_method_str,
#endif
				volume_type_str,
				ba_addr_any ? "ANY" : &ba_str[2],
				ba_profile_a2dp ? "A2DP" : "SCO");

		free(ba_str);
	}

	ba_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded",
			"path_namespace='/org/bluealsa'");
	ba_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved",
			"path_namespace='/org/bluealsa'");
	ba_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged",
			"arg0='"BLUEALSA_INTERFACE_PCM"'");

	if (!dbus_connection_add_filter(dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
		error("Couldn't add D-Bus filter: %s", err.message);
		return EXIT_FAILURE;
	}

	if (!ba_dbus_pcm_get_all(&dbus_ctx, &ba_pcms, &ba_pcms_count, &err))
		warn("Couldn't get BlueALSA PCM list: %s", err.message);

	for (size_t i = 0; i < ba_pcms_count; i++)
		supervise_io_worker(&ba_pcms[i]);

	struct sigaction sigact = {
		.sa_handler = main_loop_stop,
		.sa_flags = SA_RESETHAND };
	/* Call to these handlers restores the default action, so on the
	 * second call the program will be forcefully terminated. */
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	debug("Starting main loop");
	for (;;) {

		struct pollfd fds[10] = {
			{ main_loop_quit_event_fd, POLLIN, 0 } };
		nfds_t nfds = ARRAYSIZE(fds) - 1;

		if (!ba_dbus_connection_poll_fds(&dbus_ctx, &fds[1], &nfds)) {
			error("Couldn't get D-Bus connection file descriptors");
			return EXIT_FAILURE;
		}

		if (poll(fds, nfds + 1, -1) == -1 &&
				errno == EINTR)
			continue;

		if (fds[0].revents & POLLIN)
			break;

		if (ba_dbus_connection_poll_dispatch(&dbus_ctx, &fds[1], nfds))
			while (dbus_connection_dispatch(dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
				continue;

	}

	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] != NULL)
			io_worker_stop(workers[i]);
	/* When all workers are stopped, we can safely free the resources
	 * in a lockless manner without risking any race conditions. */
	for (size_t i = 0; i < workers_size; i++)
		if (workers[i] != NULL)
			io_worker_destroy(workers[i]);

	ba_dbus_connection_ctx_free(&dbus_ctx);
	return EXIT_SUCCESS;
}
