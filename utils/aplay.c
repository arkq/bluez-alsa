/*
 * BlueALSA - aplay.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <gio/gio.h>

#include "shared/ctl-client.h"
#include "shared/log.h"

/* Casting wrapper for the brevity's sake. */
#define CANCEL_ROUTINE(f) ((void (*)(void *))(f))

struct pcm_worker {
	struct ba_msg_transport transport;
	pthread_t thread;
	snd_pcm_t *pcm;
	/* file descriptor of BlueALSA */
	int ba_fd;
	/* file descriptor of PCM FIFO */
	int pcm_fd;
	/* if true, worker is marked for eviction */
	bool eviction;
	/* if true, playback is active */
	bool active;
	/* human-readable BT address */
	char addr[18];
};

static unsigned int verbose = 0;
static const char *device = "default";
static const char *ba_interface = "hci0";
static unsigned int pcm_buffer_time = 500000;
static unsigned int pcm_period_time = 100000;
static enum ba_pcm_type ba_type = BA_PCM_TYPE_A2DP;
static bool pcm_mixer = true;

static GDBusConnection *dbus = NULL;

static pthread_rwlock_t workers_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct pcm_worker *workers = NULL;
static size_t workers_count = 0;
static size_t workers_size = 0;

static bool main_loop_on = true;
static void main_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);

	main_loop_on = false;
}

static int set_hw_params(snd_pcm_t *pcm, int channels, int rate,
		unsigned int *buffer_time, unsigned int *period_time, char **msg) {

	const snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
	const snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
	snd_pcm_hw_params_t *params;
	char buf[256];
	int dir;
	int err;

	snd_pcm_hw_params_alloca(&params);

	if ((err = snd_pcm_hw_params_any(pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "Set all possible ranges: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_access(pcm, params, access)) != 0) {
		snprintf(buf, sizeof(buf), "Set assess type: %s: %s", snd_strerror(err), snd_pcm_access_name(access));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_format(pcm, params, format)) != 0) {
		snprintf(buf, sizeof(buf), "Set format: %s: %s", snd_strerror(err), snd_pcm_format_name(format));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_channels(pcm, params, channels)) != 0) {
		snprintf(buf, sizeof(buf), "Set channels: %s: %d", snd_strerror(err), channels);
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_rate(pcm, params, rate, 0)) != 0) {
		snprintf(buf, sizeof(buf), "Set sampling rate: %s: %d", snd_strerror(err), rate);
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, params, buffer_time, &dir)) != 0) {
		snprintf(buf, sizeof(buf), "Set buffer time: %s: %u", snd_strerror(err), *buffer_time);
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_period_time_near(pcm, params, period_time, &dir)) != 0) {
		snprintf(buf, sizeof(buf), "Set period time: %s: %u", snd_strerror(err), *period_time);
		goto fail;
	}
	if ((err = snd_pcm_hw_params(pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	return 0;

fail:
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static int set_sw_params(snd_pcm_t *pcm, snd_pcm_uframes_t buffer_size,
		snd_pcm_uframes_t period_size, char **msg) {

	snd_pcm_sw_params_t *params;
	char buf[256];
	int err;

	snd_pcm_sw_params_alloca(&params);

	if ((err = snd_pcm_sw_params_current(pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "Get current params: %s", snd_strerror(err));
		goto fail;
	}

	/* start the transfer when the buffer is full (or almost full) */
	snd_pcm_uframes_t threshold = (buffer_size / period_size) * period_size;
	if ((err = snd_pcm_sw_params_set_start_threshold(pcm, params, threshold)) != 0) {
		snprintf(buf, sizeof(buf), "Set start threshold: %s: %lu", snd_strerror(err), threshold);
		goto fail;
	}

	/* allow the transfer when at least period_size samples can be processed */
	if ((err = snd_pcm_sw_params_set_avail_min(pcm, params, period_size)) != 0) {
		snprintf(buf, sizeof(buf), "Set avail min: %s: %lu", snd_strerror(err), period_size);
		goto fail;
	}

	if ((err = snd_pcm_sw_params(pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	return 0;

fail:
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static struct pcm_worker *get_active_worker(void) {

	struct pcm_worker *w = NULL;
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

static int pause_device_player(const bdaddr_t *dev) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	char obj[64];
	int ret = 0;

	sprintf(obj, "/org/bluez/%s/dev_%2.2X_%2.2X_%2.2X_%2.2X_%2.2X_%2.2X/player0",
			ba_interface, dev->b[5], dev->b[4], dev->b[3], dev->b[2], dev->b[1], dev->b[0]);
	msg = g_dbus_message_new_method_call("org.bluez", obj, "org.bluez.MediaPlayer1", "Pause");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;
	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	debug("Requested playback pause");
	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		debug("Couldn't pause player: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

static void pcm_worker_routine_exit(struct pcm_worker *worker) {
	if (worker->pcm_fd != -1) {
		bluealsa_close_transport(worker->ba_fd, &worker->transport);
		close(worker->pcm_fd);
		worker->pcm_fd = -1;
	}
	if (worker->ba_fd != -1) {
		close(worker->ba_fd);
		worker->ba_fd = -1;
	}
	if (worker->pcm != NULL) {
		snd_pcm_close(worker->pcm);
		worker->pcm = NULL;
	}
	worker->eviction = true;
	debug("Exiting PCM worker %s", worker->addr);
}

static void *pcm_worker_routine(void *arg) {
	struct pcm_worker *w = (struct pcm_worker *)arg;

	unsigned int buffer_time = pcm_buffer_time;
	unsigned int period_time = pcm_period_time;
	char *buffer = NULL;
	char *msg = NULL;
	int err;

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	pthread_cleanup_push(CANCEL_ROUTINE(pcm_worker_routine_exit), w);
	pthread_cleanup_push(CANCEL_ROUTINE(free), buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), msg);

	if ((err = snd_pcm_open(&w->pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) != 0) {
		error("Couldn't open PCM: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = set_hw_params(w->pcm, w->transport.channels, w->transport.sampling,
					&buffer_time, &period_time, &msg)) != 0) {
		error("Couldn't set HW parameters: %s", msg);
		goto fail;
	}

	snd_pcm_uframes_t buffer_size, period_size;
	if ((err = snd_pcm_get_params(w->pcm, &buffer_size, &period_size)) != 0) {
		error("Couldn't get PCM parameters: %s", snd_strerror(err));
		goto fail;
	}

	if (verbose >= 2)
		printf("Used configuration for %s:\n"
				"  PCM buffer time: %u us (%zu bytes)\n"
				"  PCM period time: %u us (%zu bytes)\n"
				"  Sampling rate: %u Hz\n"
				"  Channels: %u\n",
				w->addr,
				buffer_time, snd_pcm_frames_to_bytes(w->pcm, buffer_size),
				period_time, snd_pcm_frames_to_bytes(w->pcm, period_size),
				w->transport.sampling, w->transport.channels);

	if ((err = set_sw_params(w->pcm, buffer_size, period_size, &msg)) != 0) {
		error("Couldn't set SW parameters: %s", msg);
		goto fail;
	}

	if ((err = snd_pcm_prepare(w->pcm)) != 0) {
		error("Couldn't prepare PCM: %s", snd_strerror(err));
		goto fail;
	}

	ssize_t frame_size = snd_pcm_frames_to_bytes(w->pcm, 1);
	buffer = malloc(period_size * frame_size);
	char *buffer_tail = buffer;

	if (buffer == NULL) {
		error("Couldn't allocate PCM buffer");
		goto fail;
	}

	if ((w->ba_fd = bluealsa_open(ba_interface)) == -1) {
		error("Couldn't open BlueALSA: %s", strerror(errno));
		goto fail;
	}

	w->transport.stream = BA_PCM_STREAM_CAPTURE;
	if ((w->pcm_fd = bluealsa_open_transport(w->ba_fd, &w->transport)) == -1) {
		error("Couldn't open PCM FIFO: %s", strerror(errno));
		goto fail;
	}

	/* These variables determine how and when the pause command will be send
	 * to the device player. In order not to flood BT connection with AVRCP
	 * packets, we are going to send pause command every 0.5 second. */
	size_t pause_threshold = snd_pcm_frames_to_bytes(w->pcm, w->transport.sampling / 2);
	size_t pause_counter = 0;
	size_t pause_bytes = 0;

	struct pollfd pfds[] = {{ w->pcm_fd, POLLIN, 0 }};
	int timeout = -1;

	debug("Starting PCM loop");
	while (main_loop_on) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		size_t buffer_len = period_size * frame_size - (buffer_tail - buffer);
		ssize_t ret;

		/* Reading from the FIFO won't block unless there is an open connection
		 * on the writing side. However, the server does not open PCM FIFO until
		 * a transport is created. With the A2DP, the transport is created when
		 * some clients (BT device) requests audio transfer. */
		switch (poll(pfds, sizeof(pfds) / sizeof(*pfds), timeout)) {
		case -1:
			if (errno == EINTR)
				continue;
		case 0:
			debug("Device marked as inactive: %s", w->addr);
			pause_counter = pause_bytes = 0;
			buffer_tail = buffer;
			w->active = false;
			timeout = -1;
			continue;
		}

		/* FIFO has been terminated on the writing side */
		if (pfds[0].revents & POLLHUP)
			break;

		if ((ret = read(w->pcm_fd, buffer_tail, buffer_len)) == -1) {
			if (errno == EINTR)
				continue;
			error("PCM FIFO read error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* If PCM mixer is disabled, check whether we should play audio. */
		if (!pcm_mixer) {
			struct pcm_worker *worker = get_active_worker();
			if (worker != NULL && worker != w) {
				if (pause_counter < 5 && (pause_bytes += ret) > pause_threshold) {
					if (pause_device_player(&w->transport.addr) == -1)
						/* pause command does not work, stop further requests */
						pause_counter = 5;
					pause_counter++;
					pause_bytes = 0;
					timeout = 100;
				}
				continue;
			}
		}

		/* mark device as active and set timeout to 500ms */
		w->active = true;
		timeout = 500;

		/* calculate the overall number of frames in the buffer */
		snd_pcm_uframes_t frames = ((buffer_tail - buffer) + ret) / frame_size;

		if ((err = snd_pcm_writei(w->pcm, buffer, frames)) < 0)
			switch (-err) {
			case EPIPE:
				debug("An underrun has occurred");
				snd_pcm_prepare(w->pcm);
				usleep(50000);
				err = 0;
				break;
			default:
				error("Couldn't write to PCM: %s", snd_strerror(err));
				goto fail;
			}

		size_t writei_len = err * frame_size;
		buffer_tail = buffer;

		/* move leftovers to the beginning and reposition tail */
		if ((size_t)ret > writei_len) {
			memmove(buffer, buffer + writei_len, ret - writei_len);
			buffer_tail += ret - writei_len;
		}

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVvi:d:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "hci", required_argument, NULL, 'i' },
		{ "pcm", required_argument, NULL, 'd' },
		{ "pcm-buffer-time", required_argument, NULL, 3 },
		{ "pcm-period-time", required_argument, NULL, 4 },
		{ "profile-a2dp", no_argument, NULL, 1 },
		{ "profile-sco", no_argument, NULL, 2 },
		{ "single-audio", no_argument, NULL, 5 },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <BT-ADDR>...\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -v, --verbose\t\tmake output more verbose\n"
					"  -i, --hci=hciX\tHCI device to use\n"
					"  -d, --pcm=NAME\tPCM device to use\n"
					"  --pcm-buffer-time=INT\tPCM buffer time\n"
					"  --pcm-period-time=INT\tPCM period time\n"
					"  --profile-a2dp\tuse A2DP profile\n"
					"  --profile-sco\t\tuse SCO profile\n"
					"  --single-audio\tsingle audio mode\n"
					"\nNote:\n"
					"If one wants to receive audio from more than one Bluetooth device, it is\n"
					"possible to specify more than one MAC address. By specifying any/empty MAC\n"
					"address (00:00:00:00:00:00), one will allow connections from any Bluetooth\n"
					"device.\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'v' /* --verbose */ :
			verbose++;
			break;

		case 'i' /* --hci */ :
			ba_interface = optarg;
			break;
		case 'd' /* --pcm */ :
			device = optarg;
			break;

		case 1 /* --profile-a2dp */ :
			ba_type = BA_PCM_TYPE_A2DP;
			break;
		case 2 /* --profile-sco */ :
			ba_type = BA_PCM_TYPE_SCO;
			break;

		case 3 /* --pcm-buffer-time */ :
			pcm_buffer_time = atoi(optarg);
			break;
		case 4 /* --pcm-period-time */ :
			pcm_period_time = atoi(optarg);
			break;

		case 5 /* --single-audio */ :
			pcm_mixer = false;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (optind == argc)
		goto usage;

	log_open(argv[0], false, false);

	bdaddr_t *ba_addrs = NULL;
	size_t ba_addrs_count = 0;
	bool ba_addr_any = false;

	int status = EXIT_SUCCESS;
	int ba_fd = -1;
	size_t i;

	ba_addrs_count = argc - optind;
	if ((ba_addrs = malloc(sizeof(*ba_addrs) * ba_addrs_count)) == NULL) {
		error("Couldn't allocate memory for BT addresses");
		goto fail;
	}
	for (i = 0; i < ba_addrs_count; i++) {
		if (str2ba(argv[i + optind], &ba_addrs[i]) != 0) {
			error("Invalid BT device address: %s", argv[i + optind]);
			goto fail;
		}
		if (bacmp(&ba_addrs[i], BDADDR_ANY) == 0)
			ba_addr_any = true;
	}

	if (verbose >= 1) {

		char *ba_str = malloc(19 * ba_addrs_count + 1);
		char *tmp = ba_str;
		size_t i;

		for (i = 0; i < ba_addrs_count; i++, tmp += 19)
			ba2str(&ba_addrs[i], stpcpy(tmp, ", "));

		printf("Selected configuration:\n"
				"  HCI device: %s\n"
				"  PCM device: %s\n"
				"  PCM buffer time: %u us\n"
				"  PCM period time: %u us\n"
				"  Bluetooth device(s): %s\n"
				"  Profile: %s\n",
				ba_interface, device, pcm_buffer_time, pcm_period_time,
				ba_addr_any ? "ANY" : &ba_str[2],
				ba_type == BA_PCM_TYPE_A2DP ? "A2DP" : "SCO");

		free(ba_str);
	}

	GError *err = NULL;
	if ((dbus = g_dbus_connection_new_for_address_sync(
					g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, NULL),
					G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
					G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
					NULL, NULL, &err)) == NULL) {
		error("Couldn't obtain D-Bus connection: %s", err->message);
		goto fail;
	}

	if ((ba_fd = bluealsa_open(ba_interface)) == -1) {
		error("BlueALSA connection failed: %s", strerror(errno));
		goto fail;
	}

	if (bluealsa_subscribe(ba_fd, BA_EVENT_TRANSPORT_ADDED | BA_EVENT_TRANSPORT_REMOVED) == -1) {
		error("BlueALSA subscription failed: %s", strerror(errno));
		goto fail;
	}

	struct sigaction sigact = { .sa_handler = main_loop_stop };
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	debug("Starting main loop");
	goto init;

	while (main_loop_on) {

		struct ba_msg_event event;
		struct ba_msg_transport *transports;
		ssize_t ret;
		size_t i;

		struct pollfd pfds[] = {{ ba_fd, POLLIN, 0 }};
		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1 && errno == EINTR)
			continue;

		while ((ret = recv(ba_fd, &event, sizeof(event), MSG_DONTWAIT)) == -1 && errno == EINTR)
			continue;
		if (ret != sizeof(event)) {
			error("Couldn't read event: %s", strerror(ret == -1 ? errno : EBADMSG));
			goto fail;
		}

init:
		debug("Fetching available transports");
		if ((ret = bluealsa_get_transports(ba_fd, &transports)) == -1) {
			error("Couldn't get transports: %s", strerror(errno));
			goto fail;
		}

		for (i = 0; i < workers_count; i++)
			workers[i].eviction = true;

		for (i = 0; i < (unsigned)ret; i++) {

			size_t ii;

			/* filter available transports by BT address (this check is omitted if
			 * any address can be used), transport type and stream direction */
			if (transports[i].type != ba_type)
				continue;
			if (transports[i].stream != BA_PCM_STREAM_CAPTURE && transports[i].stream != BA_PCM_STREAM_DUPLEX)
				continue;
			if (!ba_addr_any) {
				bool matched = false;
				for (ii = 0; ii < ba_addrs_count; ii++)
					if (bacmp(&ba_addrs[ii], &transports[i].addr) == 0) {
						matched = true;
						break;
					}
				if (!matched)
					continue;
			}

			bool matched = false;
			for (ii = 0; ii < workers_count; ii++)
				if (bacmp(&workers[ii].transport.addr, &transports[i].addr) == 0) {
					workers[ii].eviction = false;
					matched = true;
					break;
				}

			/* start PCM worker thread */
			if (!matched) {
				workers_count++;

				if (workers_size < workers_count) {

					pthread_rwlock_wrlock(&workers_lock);

					workers_size += 4; /* coarse-grained realloc */
					if ((workers = realloc(workers, sizeof(*workers) * workers_size)) == NULL) {
						error("Couldn't (re)allocate memory for PCM workers");
						goto fail;
					}

					pthread_rwlock_unlock(&workers_lock);

				}

				struct pcm_worker *worker = &workers[workers_count - 1];
				memcpy(&worker->transport, &transports[i], sizeof(worker->transport));
				ba2str(&worker->transport.addr, worker->addr);
				worker->eviction = false;
				worker->active = false;
				worker->pcm_fd = -1;
				worker->ba_fd = -1;

				debug("Creating PCM worker %s", worker->addr);

				int ret;
				if ((ret = pthread_create(&worker->thread, NULL, pcm_worker_routine, worker)) != 0) {
					warn("Couldn't create PCM worker %s: %s", worker->addr, strerror(ret));
					workers_count--;
				}

			}

		}

		/* stop PCM workers designated for eviction */
		for (i = workers_count; i > 0; i--) {
			struct pcm_worker *worker = &workers[i - 1];
			if (worker->eviction) {
				pthread_cancel(worker->thread);
				pthread_join(worker->thread, NULL);
				memcpy(worker, &workers[workers_count - 1], sizeof(*worker));
				workers_count--;
			}
		}

	}

	goto success;

fail:
	status = EXIT_FAILURE;

success:
	if (ba_fd != -1)
		close(ba_fd);
	return status;
}
