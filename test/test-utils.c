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

int main(void) {

	test_run(test_pcm_mute_s16le);
	test_run(test_pcm_scale_s16le);

	return EXIT_SUCCESS;
}
