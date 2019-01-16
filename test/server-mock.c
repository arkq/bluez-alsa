/*
 * server-mock.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
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
#include "../src/io.c"
#undef io_thread_a2dp_sink_sbc
#include "../src/rfcomm.c"
#include "../src/transport.c"
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

static const char *device = "hci-mock";
static unsigned int timeout = 5;
static bool fuzzing = false;
static bool source = false;
static bool sink = false;
static bool sco = false;

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

int test_transport_acquire(struct ba_transport *t) {

	int bt_fds[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds) == 0);

	t->bt_fd = bt_fds[0];
	t->mtu_read = 256;
	t->mtu_write = 256;

	t->state = TRANSPORT_ACTIVE;
	assert(io_thread_create(t) == 0);

	return 0;
}

int test_transport_release(struct ba_transport *t) {
	if (t->bt_fd != -1)
		close(t->bt_fd);
	t->bt_fd = -1;
	return 0;
}

struct ba_transport *test_transport_new_a2dp(struct ba_device *d,
		const char *owner, const char *path, enum bluetooth_profile profile,
		uint16_t codec, const uint8_t *config, size_t csize) {
	if (fuzzing)
		sleep(1);
	struct ba_transport *t = transport_new_a2dp(d, owner, path, profile, codec, config, csize);
	t->acquire = test_transport_acquire;
	t->release = test_transport_release;
	return t;
}

struct ba_transport *test_transport_new_sco(struct ba_device *d,
		const char *owner, const char *path, enum bluetooth_profile profile,
		uint16_t codec) {
	if (fuzzing)
		sleep(1);
	struct ba_transport *t = transport_new_sco(d, owner, path, profile, codec);
	t->acquire = test_transport_acquire;
	t->release = test_transport_release;
	return t;
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

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hsit:F";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "device", required_argument, NULL, 'i' },
		{ "timeout", required_argument, NULL, 't' },
		{ "fuzzing", no_argument, NULL, 'F' },
		{ "source", no_argument, NULL, 1 },
		{ "sink", no_argument, NULL, 2 },
		{ "sco", no_argument, NULL, 3 },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("usage: %s [--source] [--sink] [--sco] [--device HCI] [--timeout SEC]\n", argv[0]);
			return EXIT_SUCCESS;
		case 1:
			source = true;
			break;
		case 2:
			sink = true;
			break;
		case 3:
			sco = true;
			break;
		case 'i':
			device = optarg;
			break;
		case 't':
			timeout = atoi(optarg);
			break;
		case 'F':
			fuzzing = true;
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
	bluealsa_device_insert("/device/1", d1);

	str2ba("12:34:56:9A:BC:DE", &addr);
	assert((d2 = device_new(1, &addr, "Test Device With Long Name")) != NULL);
	bluealsa_device_insert("/device/2", d2);

	if (source) {
		assert(test_transport_new_a2dp(d1, ":test", "/source/1", BLUETOOTH_PROFILE_A2DP_SOURCE,
					A2DP_CODEC_SBC, (uint8_t *)&cconfig, sizeof(cconfig)) != NULL);
		assert(test_transport_new_a2dp(d2, ":test", "/source/2", BLUETOOTH_PROFILE_A2DP_SOURCE,
					A2DP_CODEC_SBC, (uint8_t *)&cconfig, sizeof(cconfig)) != NULL);
	}

	if (sink) {
		struct ba_transport *t;
		assert((t = test_transport_new_a2dp(d1, ":test", "/sink/1", BLUETOOTH_PROFILE_A2DP_SINK,
						A2DP_CODEC_SBC, (uint8_t *)&cconfig, sizeof(cconfig))) != NULL);
		assert(t->acquire(t) == 0);
		assert((t = test_transport_new_a2dp(d2, ":test", "/sink/2", BLUETOOTH_PROFILE_A2DP_SINK,
						A2DP_CODEC_SBC, (uint8_t *)&cconfig, sizeof(cconfig))) != NULL);
		assert(t->acquire(t) == 0);
	}

	if (sco) {
		struct ba_transport *t;
		assert((t = test_transport_new_sco(d1, ":test", "/sco/1", BLUETOOTH_PROFILE_HSP_AG,
						HFP_CODEC_UNDEFINED)) != NULL);
		assert((t = test_transport_new_sco(d2, ":test", "/sco/2", BLUETOOTH_PROFILE_HFP_AG,
						HFP_CODEC_UNDEFINED)) != NULL);
		if (fuzzing) {
			t->codec = HFP_CODEC_CVSD;
			bluealsa_ctl_send_event(BA_EVENT_TRANSPORT_CHANGED, &t->device->addr,
					BA_PCM_TYPE_SCO | BA_PCM_STREAM_PLAYBACK | BA_PCM_STREAM_CAPTURE);
		}
	}

	while (timeout != 0 && main_loop_on)
		timeout = sleep(timeout);

	if (fuzzing) {
		bluealsa_device_remove("/device/1");
		sleep(1);
		bluealsa_device_remove("/device/2");
		sleep(1);
	}

	return EXIT_SUCCESS;
}
