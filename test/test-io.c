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
#include "test.inc"
#include "../src/io.c"

static a2dp_sbc_t config_sbc_44100_joint_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = MIN_BITPOOL,
	.max_bitpool = MAX_BITPOOL,
};

static struct ba_transport transport_a2dp_src_sbc = {
	.profile = TRANSPORT_PROFILE_A2DP_SOURCE,
	.codec = A2DP_CODEC_SBC,
	.config_size = sizeof(a2dp_sbc_t),
	.state = TRANSPORT_IDLE,
	.bt_fd = -1,
	.pcm_fd = -1,
};

/* Helper function for timed thread join, which takes timeout value
 * in milliseconds since the call to this function. */
static int pthread_timedjoin(pthread_t thread, void **retval, useconds_t usec) {

	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)usec * 1000;

	/* normalize timespec structure */
	ts.tv_sec += ts.tv_nsec / (long)1e9;
	ts.tv_nsec = ts.tv_nsec % (long)1e9;

	return pthread_timedjoin_np(thread, retval, &ts);
}

int test_invalid_sbc_config(void) {

	pthread_t thread;
	/* invalid SBC codec configuration */
	uint8_t codec[] = { 0xff, 0xff, 0xff, 0xff };
	transport_a2dp_src_sbc.config = (uint8_t *)&codec;

	pthread_create(&thread, NULL, io_thread_a2dp_sbc_forward, &transport_a2dp_src_sbc);
	assert(pthread_timedjoin(thread, NULL, 10e3) == 0);

	assert(test_error_count == 1);
	assert(strcmp(test_error_msg, "Cannot initialize SBC codec: Invalid argument") == 0);
	return 0;
}

int test_transport_idle(void) {

	pthread_t thread;
	transport_a2dp_src_sbc.config = (uint8_t *)&config_sbc_44100_joint_stereo;
	/* transport state is IDLE, so it should terminate immediately */
	transport_a2dp_src_sbc.state = TRANSPORT_IDLE;

	pthread_create(&thread, NULL, io_thread_a2dp_sbc_forward, &transport_a2dp_src_sbc);
	assert(pthread_timedjoin(thread, NULL, 10e3) == 0);

	assert(test_error_count == 0);
	assert(strcmp(test_error_msg, "") == 0);
	return 0;
}

int main(void) {

	test_run(test_invalid_sbc_config);
	test_run(test_transport_idle);

	return EXIT_SUCCESS;
}
