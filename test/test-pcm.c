/*
 * test-pcm.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * This program might be used to debug or check the functionality of ALSA
 * plug-ins. It should work exactly the same as the BlueALSA server. When
 * connecting to the bluealsa device, one should use "hci-test" interface.
 *
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "test.inc"
#include "utils.inc"

#include "../src/bluealsa.c"
#include "../src/ctl.c"
#define transport_acquire _transport_acquire
#include "../src/transport.c"
#undef transport_acquire
#include "../src/utils.c"

static struct ba_setup setup = {
	.hci_dev = { .name = "hci-test", },
};

static const a2dp_sbc_t config = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = MIN_BITPOOL,
	.max_bitpool = MAX_BITPOOL,
};

static char *drum_buffer;
static size_t drum_buffer_size;

static void test_pcm_setup_free(void) {
	bluealsa_ctl_free(&setup);
	bluealsa_setup_free(&setup);
}

int transport_acquire(struct ba_transport *t) {
	(void)t;
	return 0;
}

void *io_thread_a2dp_sbc_forward(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	const char *end = drum_buffer + drum_buffer_size;
	char *head = drum_buffer;
	size_t len;

	do {
		fprintf(stderr, ".");

		if (t->pcm_fifo == NULL)
			continue;

		fprintf(stderr, ":");

		if (t->pcm_fd == -1) {
			if ((t->pcm_fd = open(t->pcm_fifo, O_WRONLY | O_NONBLOCK)) == -1) {
				if (errno != ENXIO)
					error("Couldn't open FIFO: %s", strerror(errno));
				/* FIFO endpoint is not connected yet */
				continue;
			}
			/* Restore the blocking mode of our FIFO. */
			fcntl(t->pcm_fd, F_SETFL, fcntl(t->pcm_fd, F_GETFL) ^ O_NONBLOCK);
		}

		fprintf(stderr, "+");

		len = head + 512 > end ? end - head : 512;
		if (write(t->pcm_fd, head, len) == -1) {

			if (errno == EPIPE) {
				debug("FIFO endpoint has been closed: %d", t->pcm_fd);
				transport_release_pcm(t);
				continue;
			}

			error("FIFO write error: %s", strerror(errno));
		}

		head += len;
		if (head == end)
			head = drum_buffer;

	}
	while (usleep(100000) == 0);

	return NULL;
}

void *io_thread_a2dp_sbc_backward(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;
	char buffer[1024 * 4];

	while ((t->pcm_fd = open(t->pcm_fifo, O_RDONLY)) == -1)
		usleep(10000);

	do {
		fprintf(stderr, ".");

		if (read(t->pcm_fd, buffer, sizeof(buffer)) == -1)
			error("FIFO read error: %s", strerror(errno));

	}
	while (usleep(10000) == 0);

	return NULL;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hbft:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "backward", no_argument, NULL, 'b' },
		{ "forward", no_argument, NULL, 'f' },
		{ "timeout", required_argument, NULL, 't' },
		{ 0, 0, 0, 0 },
	};

	int backward = 0;
	int forward = 0;
	int timeout = 5;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("usage: %s [--backward] [--forward] [--timeout SEC]\n", argv[0]);
			return EXIT_SUCCESS;
		case 'b':
			backward = 1;
			break;
		case 'f':
			forward = 1;
			break;
		case 't':
			timeout = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	assert(bluealsa_setup_init(&setup) == 0);
	if ((bluealsa_ctl_thread_init(&setup) == -1)) {
		perror("ctl_thread_init");
		return EXIT_FAILURE;
	}

	/* make sure to cleanup named pipes */
	atexit(test_pcm_setup_free);

	assert(load_file(SRCDIR "/drum.raw", &drum_buffer, &drum_buffer_size) == 0);

	bdaddr_t addr;
	struct ba_device *d;

	str2ba("12:34:56:78:9A:BC", &addr);
	assert((d = device_new(&addr, "Test Device")) != NULL);
	g_hash_table_insert(setup.devices, g_strdup("/device"), d);

	if (backward) {
		struct ba_transport *t_source;
		assert((t_source = transport_new(NULL, ":test", "/source", "Backward",
						TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC,
						(uint8_t *)&config, sizeof(config))) != NULL);
		g_hash_table_insert(d->transports, g_strdup(t_source->dbus_path), t_source);
		assert(io_thread_create(t_source) == 0);
	}

	if (forward) {
		struct ba_transport *t_sink;
		assert((t_sink = transport_new(NULL, ":test", "/sink", "Forward",
						TRANSPORT_PROFILE_A2DP_SINK, A2DP_CODEC_SBC,
						(uint8_t *)&config, sizeof(config))) != NULL);
		g_hash_table_insert(d->transports, g_strdup(t_sink->dbus_path), t_sink);
		assert(io_thread_create(t_sink) == 0);
	}

	sleep(timeout);
	return EXIT_SUCCESS;
}
