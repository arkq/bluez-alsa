/*
 * test-io.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include "a2dp.inc"
#include "test.inc"
#include "utils.inc"
#include "../src/io.c"
#include "../src/utils.c"

static const a2dp_sbc_t config_sbc_44100_joint_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = MIN_BITPOOL,
	.max_bitpool = MAX_BITPOOL,
};

/**
 * Helper function for timed thread join.
 *
 * This function takes the timeout value in milliseconds. */
static int pthread_timedjoin(pthread_t thread, void **retval, useconds_t usec) {

	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)usec * 1000;

	/* normalize timespec structure */
	ts.tv_sec += ts.tv_nsec / (long)1e9;
	ts.tv_nsec = ts.tv_nsec % (long)1e9;

	return pthread_timedjoin_np(thread, retval, &ts);
}

int test_a2dp_sbc_invalid_setup(void) {

	const uint8_t codec[] = { 0xff, 0xff, 0xff, 0xff };
	struct ba_transport transport = {
		.profile = TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_SBC,
		.config = (uint8_t *)&codec,
		.config_size = sizeof(a2dp_sbc_t),
		.state = TRANSPORT_IDLE,
		.bt_fd = -1,
	};

	pthread_t thread;

	pthread_create(&thread, NULL, io_thread_a2dp_sbc_forward, &transport);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 1);
	assert(strcmp(test_error_msg, "Invalid BT socket: -1") == 0);

	transport.bt_fd = 0;

	pthread_create(&thread, NULL, io_thread_a2dp_sbc_forward, &transport);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 2);
	assert(strcmp(test_error_msg, "Invalid reading MTU: 0") == 0);

	transport.mtu_read = 475;

	pthread_create(&thread, NULL, io_thread_a2dp_sbc_forward, &transport);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 3);
	assert(strcmp(test_error_msg, "Couldn't initialize SBC codec: Invalid argument") == 0);

	transport.config = (uint8_t *)&config_sbc_44100_joint_stereo;
	*test_error_msg = '\0';

	pthread_create(&thread, NULL, io_thread_a2dp_sbc_forward, &transport);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 3);
	assert(strcmp(test_error_msg, "") == 0);

	return 0;
}

int test_a2dp_sbc_decoding(void) {

	int bt_fds[2];
	int pcm_fds[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, bt_fds) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_fds) == 0);

	struct ba_transport transport = {
		.profile = TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_SBC,
		.config = (uint8_t *)&config_sbc_44100_joint_stereo,
		.config_size = sizeof(a2dp_sbc_t),
		.state = TRANSPORT_ACTIVE,
		.pcm_fifo = "/force-decoding",
		.pcm_fd = pcm_fds[0],
		.mtu_read = 475,
		.bt_fd = bt_fds[1],
	};

	pthread_t thread;
	char *buffer;
	size_t size;

	pthread_create(&thread, NULL, io_thread_a2dp_sbc_forward, &transport);

	assert(load_file(SRCDIR "/drum.raw", &buffer, &size) == 0);
	assert(a2dp_write_sbc(bt_fds[0], &config_sbc_44100_joint_stereo, buffer, size) == 0);
	close(bt_fds[0]);

	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 0);

	free(buffer);
	return 0;
}

int test_a2dp_sbc_encoding(void) {

	int bt_fds[2];
	int pcm_fds[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, bt_fds) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_fds) == 0);

	struct ba_transport transport = {
		.profile = TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_SBC,
		.config = (uint8_t *)&config_sbc_44100_joint_stereo,
		.config_size = sizeof(a2dp_sbc_t),
		.state = TRANSPORT_ACTIVE,
		.pcm_fd = pcm_fds[1],
		.bt_fd = bt_fds[0],
	};

	pthread_t thread;
	char *buffer;
	size_t size;

	pthread_create(&thread, NULL, io_thread_a2dp_sbc_backward, &transport);

	assert(load_file(SRCDIR "/drum.raw", &buffer, &size) == 0);
	assert(write(pcm_fds[0], buffer, size) == (signed)size);
	close(pcm_fds[0]);

	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 0);

	free(buffer);
	return 0;
}

int main(void) {

	test_run(test_a2dp_sbc_invalid_setup);
	test_run(test_a2dp_sbc_decoding);
	test_run(test_a2dp_sbc_encoding);

	return EXIT_SUCCESS;
}
