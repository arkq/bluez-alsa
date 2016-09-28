/*
 * test-utils.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp.inc"
#include "test.inc"
#include "../src/utils.c"

int test_pcm_mute_s16le(void) {

	const int16_t mute[] = { 0x0000, 0x0000, 0x0000, 0x0000 };
	int16_t in[] = { 0x1234, 0x2345, 0x3456, 0x4567 };

	assert(snd_pcm_mute_s16le(in, sizeof(in)) == 4);
	assert(memcmp(in, mute, sizeof(mute)) == 0);

	return 0;
}

int test_pcm_scale_s16le(void) {

	const int16_t mute[] = { 0x0000, 0x0000, 0x0000, 0x0000 };
	const int16_t half[] = { 0x1234 / 2, 0x2345 / 2, (int16_t)0xBCDE / 2, (int16_t)0xCDEF / 2 };
	const int16_t in[] = { 0x1234, 0x2345, 0xBCDE, 0xCDEF };
	int16_t tmp[sizeof(in) / sizeof(*in)];

	memmove(tmp, in, sizeof(tmp));
	assert(snd_pcm_scale_s16le(tmp, sizeof(tmp), 0) == 4);
	assert(memcmp(tmp, mute, sizeof(mute)) == 0);

	memmove(tmp, in, sizeof(tmp));
	assert(snd_pcm_scale_s16le(tmp, sizeof(tmp), 100) == 4);
	assert(memcmp(tmp, in, sizeof(in)) == 0);

	memmove(tmp, in, sizeof(tmp));
	assert(snd_pcm_scale_s16le(tmp, sizeof(tmp), 50) == 4);
	assert(memcmp(tmp, half, sizeof(half)) == 0);

	return 0;
}

int test_difftimespec(void) {

	struct timespec ts1, ts2, ts;

	ts1.tv_sec = ts2.tv_sec = 12345;
	ts1.tv_nsec = ts2.tv_nsec = 67890;
	assert(difftimespec(&ts1, &ts2, &ts) == 0);
	assert(ts.tv_sec == 0 && ts.tv_nsec == 0);

	ts1.tv_sec = 10;
	ts1.tv_nsec = 100000000;
	ts2.tv_sec = 10;
	ts2.tv_nsec = 500000000;
	assert(difftimespec(&ts1, &ts2, &ts) > 0);
	assert(ts.tv_sec == 0 && ts.tv_nsec == 400000000);

	ts1.tv_sec = 10;
	ts1.tv_nsec = 800000000;
	ts2.tv_sec = 12;
	ts2.tv_nsec = 100000000;
	assert(difftimespec(&ts1, &ts2, &ts) > 0);
	assert(ts.tv_sec == 1 && ts.tv_nsec == 300000000);

	ts1.tv_sec = 10;
	ts1.tv_nsec = 500000000;
	ts2.tv_sec = 10;
	ts2.tv_nsec = 100000000;
	assert(difftimespec(&ts1, &ts2, &ts) < 0);
	assert(ts.tv_sec == 0 && ts.tv_nsec == 400000000);

	ts1.tv_sec = 12;
	ts1.tv_nsec = 100000000;
	ts2.tv_sec = 10;
	ts2.tv_nsec = 800000000;
	assert(difftimespec(&ts1, &ts2, &ts) < 0);
	assert(ts.tv_sec == 1 && ts.tv_nsec == 300000000);

	return 0;
}

int main(void) {

	test_run(test_pcm_mute_s16le);
	test_run(test_pcm_scale_s16le);
	test_run(test_difftimespec);

	return EXIT_SUCCESS;
}
