/*
 * server-mock.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * This program might be used to debug or check the functionality of ALSA
 * plug-ins. It should work exactly the same as the BlueALSA server. When
 * connecting to the bluealsa device, one should use "hci-mock" interface.
 *
 */

#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "inc/sine.inc"

#include "../src/bluealsa.c"
#include "../src/at.c"
#include "../src/ctl.c"
#include "../src/io.h"
#define io_thread_a2dp_sink_sbc _io_thread_a2dp_sink_sbc
#define io_thread_a2dp_source_sbc _io_thread_a2dp_source_sbc
#include "../src/io.c"
#undef io_thread_a2dp_sink_sbc
#undef io_thread_a2dp_source_sbc
#include "../src/rfcomm.c"
#define transport_acquire_bt_a2dp _transport_acquire_bt_a2dp
#include "../src/transport.c"
#undef transport_acquire_bt_a2dp
#include "../src/utils.c"
#include "../src/shared/ffb.c"
#include "../src/shared/log.c"
#include "../src/shared/rt.c"

static const a2dp_sbc_t cconfig = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

static void test_pcm_setup_free(void) {
	bluealsa_ctl_free();
	bluealsa_config_free();
}

static bool main_loop_on = true;
static void test_pcm_setup_free_handler(int sig) {
	(void)(sig);
	main_loop_on = false;
	test_pcm_setup_free();
}

static int sigusr1_count = 0;
static int sigusr2_count = 0;
static void test_sigusr_handler(int sig) {
	switch (sig) {
	case SIGUSR1:
		debug("Dispatching SIGUSR1");
		sigusr1_count++;
		break;
	case SIGUSR2:
		debug("Dispatching SIGUSR2");
		sigusr2_count++;
		break;
	default:
		error("Unsupported signal: %d", sig);
	}
}

int transport_acquire_bt_a2dp(struct ba_transport *t) {
	t->delay = 1; /* suppress delay check trigger */
	t->state = TRANSPORT_ACTIVE;
	assert(io_thread_create(t) == 0);
	return 0;
}

void *io_thread_a2dp_sink_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;
	pthread_cleanup_push(PTHREAD_CLEANUP(transport_pthread_cleanup), t);

	struct asrsync asrs = { .frames = 0 };
	int16_t buffer[1024 * 2];
	int x = 0;

	while (sigusr1_count == 0) {

		if (t->a2dp.pcm.fd == -1) {
			usleep(10000);
			continue;
		}

		fprintf(stderr, ".");

		if (asrs.frames == 0)
			asrsync_init(&asrs, transport_get_sampling(t));

		int samples = sizeof(buffer) / sizeof(int16_t);
		x = snd_pcm_sine_s16le(buffer, samples, 2, x, 0.01);

		if (io_thread_write_pcm(&t->a2dp.pcm, buffer, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		asrsync_sync(&asrs, samples / 2);
	}

	pthread_cleanup_pop(1);
	return NULL;
}

void *io_thread_a2dp_source_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;
	pthread_cleanup_push(PTHREAD_CLEANUP(transport_pthread_cleanup), t);

	struct asrsync asrs = { .frames = 0 };
	int16_t buffer[1024 * 2];
	ssize_t samples;

	while (sigusr2_count == 0) {
		fprintf(stderr, ".");

		if (asrs.frames == 0)
			asrsync_init(&asrs, transport_get_sampling(t));

		const size_t in_samples = sizeof(buffer) / sizeof(int16_t);
		if ((samples = io_thread_read_pcm(&t->a2dp.pcm, buffer, in_samples)) <= 0) {
			if (samples == -1)
				error("FIFO read error: %s", strerror(errno));
			break;
		}

		asrsync_sync(&asrs, samples / 2);
	}

	pthread_cleanup_pop(1);
	return NULL;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hsit:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "device", required_argument, NULL, 'i' },
		{ "timeout", required_argument, NULL, 't' },
		{ "source", no_argument, NULL, 1 },
		{ "sink", no_argument, NULL, 2 },
		{ 0, 0, 0, 0 },
	};

	const char *device = "hci-mock";
	unsigned int timeout = 5;
	bool source = false;
	bool sink = false;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("usage: %s [--source] [--sink] [--device HCI] [--timeout SEC]\n", argv[0]);
			return EXIT_SUCCESS;
		case 1:
			source = true;
			break;
		case 2:
			sink = true;
			break;
		case 'i':
			device = optarg;
			break;
		case 't':
			timeout = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	/* emulate dummy test HCI device */
	strncpy(config.hci_dev.name, device, sizeof(config.hci_dev.name) - 1);

	assert(bluealsa_config_init() == 0);
	assert(bluealsa_ctl_thread_init() == 0);

	/* make sure to cleanup named pipes */
	struct sigaction sigact = { .sa_handler = test_pcm_setup_free_handler };
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	atexit(test_pcm_setup_free);

	/* receive EPIPE error code */
	sigact.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sigact, NULL);

	/* register USR signals handler */
	sigact.sa_handler = test_sigusr_handler;
	sigaction(SIGUSR1, &sigact, NULL);
	sigaction(SIGUSR2, &sigact, NULL);

	bdaddr_t addr;
	struct ba_device *d1, *d2;

	/* Connect two devices with the same name, but different MAC addresses.
	 * This test will ensure, that it is possible to launch mixer plug-in. */

	str2ba("12:34:56:78:9A:BC", &addr);
	assert((d1 = device_new(1, &addr, "Test Device With Long Name")) != NULL);
	g_hash_table_insert(config.devices, g_strdup("/device/1"), d1);

	str2ba("12:34:56:9A:BC:DE", &addr);
	assert((d2 = device_new(1, &addr, "Test Device With Long Name")) != NULL);
	g_hash_table_insert(config.devices, g_strdup("/device/2"), d2);

	if (source) {
		assert(transport_new_a2dp(d1, ":test", "/source/1", BLUETOOTH_PROFILE_A2DP_SOURCE,
					A2DP_CODEC_SBC, (uint8_t *)&cconfig, sizeof(cconfig)) != NULL);
		assert(transport_new_a2dp(d2, ":test", "/source/2", BLUETOOTH_PROFILE_A2DP_SOURCE,
					A2DP_CODEC_SBC, (uint8_t *)&cconfig, sizeof(cconfig)) != NULL);
	}

	if (sink) {
		struct ba_transport *t;
		assert((t = transport_new_a2dp(d1, ":test", "/sink/1", BLUETOOTH_PROFILE_A2DP_SINK,
						A2DP_CODEC_SBC, (uint8_t *)&cconfig, sizeof(cconfig))) != NULL);
		assert(transport_acquire_bt_a2dp(t) == 0);
		assert((t = transport_new_a2dp(d2, ":test", "/sink/2", BLUETOOTH_PROFILE_A2DP_SINK,
						A2DP_CODEC_SBC, (uint8_t *)&cconfig, sizeof(cconfig))) != NULL);
		assert(transport_acquire_bt_a2dp(t) == 0);
	}

	while (timeout != 0 && main_loop_on)
		timeout = sleep(timeout);

	return EXIT_SUCCESS;
}
