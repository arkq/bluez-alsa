/*
 * test-utils.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "inc/test.inc"
#include "../src/utils.c"
#include "../src/shared/ffb.c"
#include "../src/shared/rt.c"

int test_dbus_profile_object_path(void) {

	static const struct {
		enum bluetooth_profile profile;
		int16_t codec;
		const char *path;
	} profiles[] = {
		/* test null/invalid path */
		{ BLUETOOTH_PROFILE_NULL, -1, "/" },
		{ BLUETOOTH_PROFILE_NULL, -1, "/Invalid" },
		/* test A2DP profiles */
		{ BLUETOOTH_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC, "/A2DP/SBC/Source" },
		{ BLUETOOTH_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC, "/A2DP/SBC/Source/1" },
		{ BLUETOOTH_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC, "/A2DP/SBC/Source/2" },
		{ BLUETOOTH_PROFILE_A2DP_SINK, A2DP_CODEC_SBC, "/A2DP/SBC/Sink" },
#if ENABLE_MP3
		{ BLUETOOTH_PROFILE_A2DP_SOURCE, A2DP_CODEC_MPEG12, "/A2DP/MPEG12/Source" },
		{ BLUETOOTH_PROFILE_A2DP_SINK, A2DP_CODEC_MPEG12, "/A2DP/MPEG12/Sink" },
#endif
#if ENABLE_AAC
		{ BLUETOOTH_PROFILE_A2DP_SOURCE, A2DP_CODEC_MPEG24, "/A2DP/MPEG24/Source" },
		{ BLUETOOTH_PROFILE_A2DP_SINK, A2DP_CODEC_MPEG24, "/A2DP/MPEG24/Sink" },
#endif
#if ENABLE_APTX
		{ BLUETOOTH_PROFILE_A2DP_SOURCE, A2DP_CODEC_VENDOR_APTX, "/A2DP/APTX/Source" },
		{ BLUETOOTH_PROFILE_A2DP_SINK, A2DP_CODEC_VENDOR_APTX, "/A2DP/APTX/Sink" },
#endif
		/* test HSP/HFP profiles */
		{ BLUETOOTH_PROFILE_HSP_HS, -1, "/HSP/Headset" },
		{ BLUETOOTH_PROFILE_HSP_AG, -1, "/HSP/AudioGateway" },
		{ BLUETOOTH_PROFILE_HFP_HF, -1, "/HFP/HandsFree" },
		{ BLUETOOTH_PROFILE_HFP_AG, -1, "/HFP/AudioGateway" },
	};

	size_t i;

	for (i = 0; i < sizeof(profiles) / sizeof(*profiles); i++) {
		const char *path = g_dbus_get_profile_object_path(profiles[i].profile, profiles[i].codec);
		assert(strstr(profiles[i].path, path) == profiles[i].path);
		assert(g_dbus_object_path_to_profile(profiles[i].path) == profiles[i].profile);
		if (profiles[i].codec != -1)
			assert(g_dbus_object_path_to_a2dp_codec(profiles[i].path) == profiles[i].codec);
	}

	return 0;
}

int test_pcm_scale_s16le(void) {

	const int16_t mute[] = { 0x0000, 0x0000, 0x0000, 0x0000 };
	const int16_t half[] = { 0x1234 / 2, 0x2345 / 2, (int16_t)0xBCDE / 2, (int16_t)0xCDEF / 2 };
	const int16_t halfl[] = { 0x1234 / 2, 0x2345, (int16_t)0xBCDE / 2, 0xCDEF };
	const int16_t halfr[] = { 0x1234, 0x2345 / 2, 0xBCDE, (int16_t)0xCDEF / 2 };
	const int16_t in[] = { 0x1234, 0x2345, 0xBCDE, 0xCDEF };
	int16_t tmp[sizeof(in) / sizeof(*in)];

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, sizeof(tmp) / sizeof(*tmp), 1, 0, 0);
	assert(memcmp(tmp, mute, sizeof(mute)) == 0);

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, sizeof(tmp) / sizeof(*tmp), 1, 1.0, 1.0);
	assert(memcmp(tmp, in, sizeof(in)) == 0);

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, sizeof(tmp) / sizeof(*tmp), 1, 0.5, 0.5);
	assert(memcmp(tmp, half, sizeof(half)) == 0);

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, sizeof(tmp) / sizeof(*tmp), 2, 0.5, 1.0);
	assert(memcmp(tmp, halfl, sizeof(halfl)) == 0);

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, sizeof(tmp) / sizeof(*tmp), 2, 1.0, 0.5);
	assert(memcmp(tmp, halfr, sizeof(halfr)) == 0);

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

int test_fifo_buffer(void) {

	struct ffb ffb = { 0 };

	assert(ffb_init(&ffb, 64) == 0);
	assert(ffb.data == ffb.tail);
	assert(ffb.size == 64);

	memcpy(ffb.data, "1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ", 36);
	ffb_seek(&ffb, 36);

	assert(ffb_len_in(&ffb) == 64 - 36);
	assert(ffb_len_out(&ffb) == 36);
	assert(ffb.tail[-1] == 'Z');

	ffb_rewind(&ffb, 15);
	assert(ffb_len_in(&ffb) == 64 - (36 - 15));
	assert(ffb_len_out(&ffb) == 36 - 15);
	assert(memcmp(ffb.data, "FGHIJKLMNOPQRSTUVWXYZ", ffb_len_out(&ffb)) == 0);
	assert(ffb.tail[-1] == 'Z');

	return 0;
}

int main(void) {
	test_run(test_dbus_profile_object_path);
	test_run(test_pcm_scale_s16le);
	test_run(test_difftimespec);
	test_run(test_fifo_buffer);
	return 0;
}
