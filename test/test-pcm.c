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
#include "../src/io.h"
#define io_thread_a2dp_sink_sbc _io_thread_a2dp_sink_sbc
#define io_thread_a2dp_source_sbc _io_thread_a2dp_source_sbc
#include "../src/io.c"
#undef io_thread_a2dp_sink_sbc
#undef io_thread_a2dp_source_sbc
#define transport_acquire_bt _transport_acquire_bt
#include "../src/transport.c"
#undef transport_acquire_bt
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

int transport_acquire_bt(struct ba_transport *t) {
	(void)t;
	return 0;
}

void *io_thread_a2dp_sink_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	const char *end = drum_buffer + drum_buffer_size;
	char *head = drum_buffer;
	ssize_t len;

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	struct io_sync io_sync = {
		.sampling = transport_get_sampling(t),
	};

	while (TRANSPORT_RUN_IO_THREAD(t)) {

		if (t->pcm_fifo == NULL) {
			usleep(10000);
			continue;
		}

		if (t->pcm_fd == -1) {
			if ((t->pcm_fd = open(t->pcm_fifo, O_WRONLY | O_NONBLOCK)) == -1) {
				if (errno != ENXIO)
					error("Couldn't open FIFO: %s", strerror(errno));
				/* FIFO endpoint is not connected yet */
				usleep(10000);
				continue;
			}
			/* Restore the blocking mode of our FIFO. */
			fcntl(t->pcm_fd, F_SETFL, fcntl(t->pcm_fd, F_GETFL) & ~O_NONBLOCK);
		}

		fprintf(stderr, ".");

		if (io_sync.frames == 0)
			clock_gettime(CLOCK_MONOTONIC, &io_sync.ts0);

		if (head == end)
			head = drum_buffer;

		len = head + 1024 > end ? end - head : 1024;
		if ((len = write(t->pcm_fd, head, len)) == -1) {

			if (errno == EPIPE) {
				debug("FIFO endpoint has been closed: %d", t->pcm_fd);
				transport_release_pcm(t);
				continue;
			}

			error("FIFO write error: %s", strerror(errno));
		}

		head += len;
		io_thread_time_sync(&io_sync, len / 2 / 2);
	}
}

void *io_thread_a2dp_source_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	while ((t->pcm_fd = open(t->pcm_fifo, O_RDONLY)) == -1)
		usleep(10000);

	char buffer[1024 * 4];
	ssize_t len;

	struct io_sync io_sync = {
		.sampling = transport_get_sampling(t),
	};

	while (TRANSPORT_RUN_IO_THREAD(t)) {
		fprintf(stderr, ".");

		if (io_sync.frames == 0)
			clock_gettime(CLOCK_MONOTONIC, &io_sync.ts0);

		if ((len = read(t->pcm_fd, buffer, sizeof(buffer))) == -1) {
			error("FIFO read error: %s", strerror(errno));
			return NULL;
		}

		io_thread_time_sync(&io_sync, len / 2 / 2);
	}
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hsit:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "source", no_argument, NULL, 's' },
		{ "sink", no_argument, NULL, 'i' },
		{ "timeout", required_argument, NULL, 't' },
		{ 0, 0, 0, 0 },
	};

	int source = 0;
	int sink = 0;
	int timeout = 5;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("usage: %s [--source] [--sink] [--timeout SEC]\n", argv[0]);
			return EXIT_SUCCESS;
		case 's':
			source = 1;
			break;
		case 'i':
			sink = 1;
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

	bdaddr_t addr;
	struct ba_device *d;

	str2ba("12:34:56:78:9A:BC", &addr);
	assert((d = device_new(&addr, "Test Device")) != NULL);
	g_hash_table_insert(setup.devices, g_strdup("/device"), d);

	if (source) {
		struct ba_transport *t_source;
		assert((t_source = transport_new(NULL, ":test", "/source", "Source",
						TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC,
						(uint8_t *)&config, sizeof(config))) != NULL);
		g_hash_table_insert(d->transports, g_strdup(t_source->dbus_path), t_source);
		t_source->state = TRANSPORT_ACTIVE;
		assert(io_thread_create(t_source) == 0);
	}

	if (sink) {
		struct ba_transport *t_sink;
		assert((t_sink = transport_new(NULL, ":test", "/sink", "Sink",
						TRANSPORT_PROFILE_A2DP_SINK, A2DP_CODEC_SBC,
						(uint8_t *)&config, sizeof(config))) != NULL);
		g_hash_table_insert(d->transports, g_strdup(t_sink->dbus_path), t_sink);
		assert(load_file(SRCDIR "/drum.raw", &drum_buffer, &drum_buffer_size) == 0);
		t_sink->state = TRANSPORT_ACTIVE;
		assert(io_thread_create(t_sink) == 0);
	}

	sleep(timeout);
	return EXIT_SUCCESS;
}
