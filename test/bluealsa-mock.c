/*
 * bluealsa-mock.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * This program might be used to debug or check the functionality of ALSA
 * plug-ins. It should work exactly the same as the BlueALSA server.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include "a2dp-audio.h"
#include "a2dp-codecs.h"
#include "a2dp.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa-dbus.h"
#include "bluealsa-iface.h"
#include "bluealsa.h"
#include "bluez.h"
#include "codec-sbc.h"
#include "hfp.h"
#include "io.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"

#include "../src/a2dp.c"
#include "inc/dbus.inc"
#include "inc/sine.inc"

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
	.frequency = APTX_SAMPLING_FREQ_44100,
};

static const a2dp_aptx_hd_t config_aptx_hd_48000_stereo = {
	.aptx.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
	.aptx.channel_mode = APTX_CHANNEL_MODE_STEREO,
	.aptx.frequency = APTX_SAMPLING_FREQ_48000,
};

static struct ba_adapter *a = NULL;
static char service[32] = BLUEALSA_SERVICE;
static GMutex timeout_mutex = { NULL };
static GCond timeout_cond = { NULL };
static int timeout = 5;
static bool a2dp_extra_codecs = false;
static bool a2dp_source = false;
static bool a2dp_sink = false;
static bool sco_hfp = false;
static bool sco_hsp = false;
static bool dump_output = false;
static bool fuzzing = false;

static gboolean main_loop_exit_handler(void *userdata) {
	g_main_loop_quit((GMainLoop *)userdata);
	return G_SOURCE_REMOVE;
}

static gboolean main_loop_timeout_handler(void *userdata) {
	g_mutex_lock(&timeout_mutex);
	*((int *)userdata) = -1;
	g_cond_signal(&timeout_cond);
	g_mutex_unlock(&timeout_mutex);
	return G_SOURCE_REMOVE;
}

static volatile sig_atomic_t sigusr1_count = 0;
static volatile sig_atomic_t sigusr2_count = 0;
static void mock_sigusr_handler(int sig) {
	switch (sig) {
	case SIGUSR1:
		sigusr1_count++;
		debug("Dispatching SIGUSR1: %d", sigusr1_count);
		break;
	case SIGUSR2:
		sigusr2_count++;
		debug("Dispatching SIGUSR2: %d", sigusr2_count);
		break;
	default:
		error("Unsupported signal: %d", sig);
	}
}

bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s", __func__, current_dbus_sep_path); (void)sep;
	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Not supported");
	return false;
}

static void *mock_a2dp_sink(struct ba_transport_thread *th) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
	struct pollfd fds[1] = {{ th->pipe[0], POLLIN, 0 }};
	struct asrsync asrs = { .frames = 0 };
	int16_t buffer[1024 * 2];
	int x = 0;

	debug_transport_thread_loop(th, "START");
	ba_transport_thread_set_state_running(th);

	while (sigusr1_count == 0) {

		int timout = 0;
		if (t->a2dp.pcm.fd == -1 || !t->a2dp.pcm.active)
			timout = -1;

		if (poll(fds, ARRAYSIZE(fds), timout) == 1 &&
				fds[0].revents & POLLIN) {
			/* dispatch incoming event */
			enum ba_transport_thread_signal signal;
			ba_transport_thread_signal_recv(th, &signal);
			switch (signal) {
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_OPEN:
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_RESUME:
				asrs.frames = 0;
				continue;
			default:
				continue;
			}
		}

		fprintf(stderr, ".");

		if (asrs.frames == 0)
			asrsync_init(&asrs, samplerate);

		const size_t samples = ARRAYSIZE(buffer);
		x = snd_pcm_sine_s16le(buffer, samples, channels, x, 1.0 / 128);

		io_pcm_scale(&t->a2dp.pcm, buffer, samples);
		if (io_pcm_write(&t->a2dp.pcm, buffer, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		/* maintain constant speed */
		asrsync_sync(&asrs, samples / channels);

	}

	ba_transport_thread_set_state_stopping(th);
	pthread_cleanup_pop(1);
	return NULL;
}

static void *mock_bt_dump_thread(void *userdata) {

	int bt_fd = GPOINTER_TO_INT(userdata);
	FILE *f_output = NULL;
	uint8_t buffer[1024];
	ssize_t len;

	if (dump_output)
		f_output = fopen("bluealsa-mock.dump", "w");

	while ((len = read(bt_fd, buffer, sizeof(buffer))) > 0) {

		if (!dump_output)
			continue;

		for (ssize_t i = 0; i < len;i++)
			fprintf(f_output, "%02x", buffer[i]);
		fprintf(f_output, "\n");

	}

	if (f_output != NULL)
		fclose(f_output);
	close(bt_fd);
	return NULL;
}

static void mock_transport_start(struct ba_transport *t, int bt_fd) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE) {
		g_thread_unref(g_thread_new(NULL, mock_bt_dump_thread, GINT_TO_POINTER(bt_fd)));
		assert(a2dp_audio_thread_create(t) == 0);
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK) {
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			assert(ba_transport_thread_create(&t->thread_dec, mock_a2dp_sink, "ba-a2dp-sbc", true) == 0);
			break;
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			assert(ba_transport_thread_create(&t->thread_dec, mock_a2dp_sink, "ba-a2dp-aptx", true) == 0);
			break;
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			assert(ba_transport_thread_create(&t->thread_dec, mock_a2dp_sink, "ba-a2dp-aptx-hd", true) == 0);
			break;
#endif
		}
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		assert(ba_transport_start(t) == 0);
	}

}

static int mock_transport_acquire(struct ba_transport *t) {

	int bt_fds[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds) == 0);

	t->bt_fd = bt_fds[0];
	t->mtu_read = 256;
	t->mtu_write = 256;

	debug("New transport: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

	pthread_mutex_unlock(&t->bt_fd_mtx);
	mock_transport_start(t, bt_fds[1]);
	pthread_mutex_lock(&t->bt_fd_mtx);

	return 0;
}

static struct ba_device *mock_device_new(struct ba_adapter *a, const char *btmac) {
	struct ba_device *d;
	bdaddr_t addr;
	str2ba(btmac, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL)
		d = ba_device_new(a, &addr);
	return d;
}

static struct ba_transport *mock_transport_new_a2dp(const char *device_btmac,
		uint16_t profile, const struct a2dp_codec *codec, const void *configuration) {
	if (fuzzing)
		sleep(1);
	struct ba_device *d = mock_device_new(a, device_btmac);
	struct ba_transport_type type = { profile, codec->codec_id };
	const char *path = g_dbus_transport_type_to_bluez_object_path(type);
	struct ba_transport *t = ba_transport_new_a2dp(d, type, ":test", path, codec, configuration);
	fprintf(stderr, "BLUEALSA_PCM_READY=A2DP:%s:%s\n",
			device_btmac, ba_transport_codecs_a2dp_to_string(t->type.codec));
	t->acquire = mock_transport_acquire;
	if (type.profile == BA_TRANSPORT_PROFILE_A2DP_SINK)
		assert(ba_transport_acquire(t) == 0);
	ba_device_unref(d);
	return t;
}

static struct ba_transport *mock_transport_new_sco(const char *device_btmac,
		uint16_t profile, uint16_t codec) {
	if (fuzzing)
		sleep(1);
	struct ba_device *d = mock_device_new(a, device_btmac);
	struct ba_transport_type type = { profile, codec };
	const char *path = g_dbus_transport_type_to_bluez_object_path(type);
	struct ba_transport *t = ba_transport_new_sco(d, type, ":test", path, -1);
	fprintf(stderr, "BLUEALSA_PCM_READY=SCO:%s:%s\n",
			device_btmac, ba_transport_codecs_hfp_to_string(t->type.codec));
	t->acquire = mock_transport_acquire;
	ba_device_unref(d);
	return t;
}

void *mock_service_thread(void *userdata) {

	GMainLoop *loop = userdata;
	GPtrArray *tt = g_ptr_array_new();
	size_t i;

	if (a2dp_source) {

		g_ptr_array_add(tt, mock_transport_new_a2dp("12:34:56:78:9A:BC",
					BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_codec_source_sbc,
					&config_sbc_44100_stereo));

		g_ptr_array_add(tt, mock_transport_new_a2dp("23:45:67:89:AB:CD",
					BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_codec_source_sbc,
					&config_sbc_44100_stereo));

		if (a2dp_extra_codecs) {

#if ENABLE_APTX
			g_ptr_array_add(tt, mock_transport_new_a2dp("AA:BB:CC:DD:00:00",
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_codec_source_aptx,
						&config_aptx_44100_stereo));
#endif

#if ENABLE_APTX_HD
			g_ptr_array_add(tt, mock_transport_new_a2dp("AA:BB:CC:DD:88:DD",
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, &a2dp_codec_source_aptx_hd,
						&config_aptx_hd_48000_stereo));
#endif

		}

	}

	if (a2dp_sink) {

		g_ptr_array_add(tt, mock_transport_new_a2dp("12:34:56:78:9A:BC",
						BA_TRANSPORT_PROFILE_A2DP_SINK, &a2dp_codec_sink_sbc,
						&config_sbc_44100_stereo));

		g_ptr_array_add(tt, mock_transport_new_a2dp("23:45:67:89:AB:CD",
						BA_TRANSPORT_PROFILE_A2DP_SINK, &a2dp_codec_sink_sbc,
						&config_sbc_44100_stereo));

		if (a2dp_extra_codecs) {

#if ENABLE_APTX
			g_ptr_array_add(tt, mock_transport_new_a2dp("AA:BB:CC:DD:00:00",
						BA_TRANSPORT_PROFILE_A2DP_SINK, &a2dp_codec_source_aptx,
						&config_aptx_44100_stereo));
#endif

#if ENABLE_APTX_HD
			g_ptr_array_add(tt, mock_transport_new_a2dp("AA:BB:CC:DD:88:DD",
						BA_TRANSPORT_PROFILE_A2DP_SINK, &a2dp_codec_source_aptx_hd,
						&config_aptx_hd_48000_stereo));
#endif

		}

	}

	if (sco_hfp) {

		struct ba_transport *t;
		g_ptr_array_add(tt, t = mock_transport_new_sco("12:34:56:78:9A:BC",
					BA_TRANSPORT_PROFILE_HFP_AG, HFP_CODEC_UNDEFINED));

		if (fuzzing) {
			t->type.codec = HFP_CODEC_CVSD;
			bluealsa_dbus_pcm_update(&t->sco.spk_pcm,
					BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
			bluealsa_dbus_pcm_update(&t->sco.mic_pcm,
					BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
		}

	}

	if (sco_hsp) {
		g_ptr_array_add(tt, mock_transport_new_sco("23:45:67:89:AB:CD",
					BA_TRANSPORT_PROFILE_HSP_AG, HFP_CODEC_UNDEFINED));
	}

	g_mutex_lock(&timeout_mutex);
	while (timeout > 0)
		g_cond_wait(&timeout_cond, &timeout_mutex);
	g_mutex_unlock(&timeout_mutex);

	for (i = 0; i < tt->len; i++) {
		ba_transport_destroy(tt->pdata[i]);
		if (fuzzing && i % 2 == 0)
			sleep(1);
	}

	if (fuzzing)
		sleep(1);

	g_ptr_array_free(tt, TRUE);
	g_main_loop_quit(loop);
	return NULL;
}

static void dbus_name_acquired(GDBusConnection *conn, const char *name, void *userdata) {
	(void)conn;
	GMainLoop *loop = userdata;

	fprintf(stderr, "BLUEALSA_DBUS_SERVICE_NAME=%s\n", name);

	/* emulate dummy test HCI device */
	assert((a = ba_adapter_new(0)) != NULL);

	/* do not generate lots of data */
	config.sbc_quality = SBC_QUALITY_LOW;

	/* run actual BlueALSA mock thread */
	g_thread_new(NULL, mock_service_thread, loop);

}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hb:t:F";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "timeout", required_argument, NULL, 't' },
		{ "a2dp-extra-codecs", no_argument, NULL, 1 },
		{ "a2dp-source", no_argument, NULL, 2 },
		{ "a2dp-sink", no_argument, NULL, 3 },
		{ "sco-hfp", no_argument, NULL, 4 },
		{ "sco-hsp", no_argument, NULL, 5 },
		{ "dump-output", no_argument, NULL, 6 },
		{ "fuzzing", no_argument, NULL, 7 },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("Usage:\n"
					"  %s [OPTION]...\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -B, --dbus=NAME\tBlueALSA service name suffix\n"
					"  -t, --timeout=SEC\tmock server exit timeout\n"
					"  --a2dp-extra-codecs\tregister non-mandatory A2DP codecs\n"
					"  --a2dp-source\t\tregister source A2DP endpoints\n"
					"  --a2dp-sink\t\tregister sink A2DP endpoints\n"
					"  --sco-hfp\t\tregister HFP endpoints\n"
					"  --sco-hsp\t\tregister HSP endpoints\n"
					"  --dump-output\t\tdump Bluetooth transport data\n"
					"  --fuzzing\t\tmock human actions with timings\n",
					argv[0]);
			return EXIT_SUCCESS;
		case 'B' /* --dbus=NAME */ :
			snprintf(service, sizeof(service), BLUEALSA_SERVICE ".%s", optarg);
			break;
		case 't' /* --timeout=SEC */ :
			timeout = atoi(optarg);
			break;
		case 1 /* --a2dp-extra-codecs */ :
			a2dp_extra_codecs = true;
			break;
		case 2 /* -a2dp-source */ :
			a2dp_source = true;
			break;
		case 3 /* -a2dp-sink */ :
			a2dp_sink = true;
			break;
		case 4 /* --sco-hfp */ :
			sco_hfp = true;
			break;
		case 5 /* --sco-hsp */ :
			sco_hsp = true;
			break;
		case 6 /* --dump-output */ :
			dump_output = true;
			break;
		case 7 /* --fuzzing */ :
			fuzzing = true;
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	log_open(argv[0], false, true);
	assert(bluealsa_config_init() == 0);
	assert((config.dbus = g_test_dbus_connection_new_sync(NULL)) != NULL);

	/* receive EPIPE error code */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* register USR signals handler */
	sigact.sa_handler = mock_sigusr_handler;
	sigaction(SIGUSR1, &sigact, NULL);
	sigaction(SIGUSR2, &sigact, NULL);

	/* main loop with graceful termination handlers */
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add_seconds(timeout, main_loop_timeout_handler, &timeout);
	g_unix_signal_add(SIGINT, main_loop_exit_handler, loop);
	g_unix_signal_add(SIGTERM, main_loop_exit_handler, loop);

	assert(bluealsa_dbus_manager_register(NULL) != 0);
	assert(g_bus_own_name_on_connection(config.dbus, service,
				G_BUS_NAME_OWNER_FLAGS_NONE, dbus_name_acquired, NULL, loop, NULL) != 0);

	g_main_loop_run(loop);

	ba_adapter_destroy(a);
	return EXIT_SUCCESS;
}
