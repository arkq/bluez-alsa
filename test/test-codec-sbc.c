/*
 * test-codec-sbc.c
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <stdbool.h>

#include <check.h>

#include "codec-sbc.h"
#include "shared/a2dp-codecs.h"

#include <sbc/sbc.h>

#include "inc/check.inc"

CK_START_TEST(test_sbc_a2dp_get_bitpool) {

	a2dp_sbc_t conf = {
		.sampling_freq = SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL,
		.block_length = SBC_BLOCK_LENGTH_16,
		.subbands = SBC_SUBBANDS_8,
		.allocation_method = SBC_ALLOCATION_LOUDNESS,
		.max_bitpool = 250,
	};

	/* Verify XQ/XQ+ quality. */
	ck_assert_int_eq(sbc_a2dp_get_bitpool(&conf, SBC_QUALITY_XQ), 38);
	ck_assert_int_eq(sbc_a2dp_get_bitpool(&conf, SBC_QUALITY_XQPLUS), 47);

	conf.sampling_freq = SBC_SAMPLING_FREQ_48000;
	/* XQ/XQ+ requires dual-channel mode, check downgrade to high if not set. */
	ck_assert_int_eq(sbc_a2dp_get_bitpool(&conf, SBC_QUALITY_XQ), 29);

	conf.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
	/* Joint-stereo can use higher bitpool than dual-channel. */
	ck_assert_int_eq(sbc_a2dp_get_bitpool(&conf, SBC_QUALITY_HIGH), 51);

	conf.sampling_freq = SBC_SAMPLING_FREQ_32000;
	/* Check bitpool value for low sampling frequency. */
	ck_assert_int_eq(sbc_a2dp_get_bitpool(&conf, SBC_QUALITY_HIGH), 53);

	conf.sampling_freq = SBC_SAMPLING_FREQ_44100;
	/* Check bitpool value for CD-quality. */
	conf.channel_mode = SBC_CHANNEL_MODE_MONO;
	ck_assert_int_eq(sbc_a2dp_get_bitpool(&conf, SBC_QUALITY_HIGH), 31);
	conf.channel_mode = SBC_CHANNEL_MODE_STEREO;
	ck_assert_int_eq(sbc_a2dp_get_bitpool(&conf, SBC_QUALITY_HIGH), 53);

} CK_END_TEST

#if ENABLE_FASTSTREAM
CK_START_TEST(test_sbc_init_a2dp_faststream) {

	a2dp_faststream_t conf = {
		.info = A2DP_VENDOR_INFO_INIT(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
		.sampling_freq_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_44100,
		.sampling_freq_voice = FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
	};

	sbc_t sbc;

	/* Verify initialization with wrong configuration blob. */
	ck_assert_int_eq(sbc_init_a2dp_faststream(&sbc, 0, &conf, 4, false), -EINVAL);

	/* Verify initialization without required direction support. */
	conf.direction = FASTSTREAM_DIRECTION_MUSIC;
	ck_assert_int_eq(sbc_init_a2dp_faststream(&sbc, 0, &conf, sizeof(conf), true), -EINVAL);
	conf.direction = FASTSTREAM_DIRECTION_VOICE;
	ck_assert_int_eq(sbc_init_a2dp_faststream(&sbc, 0, &conf, sizeof(conf), false), -EINVAL);

	conf.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE;
	/* Verify successful initialization. */
	ck_assert_int_eq(sbc_init_a2dp_faststream(&sbc, 0, &conf, sizeof(conf), false), 0);
	ck_assert_int_eq(sbc.mode, SBC_MODE_JOINT_STEREO);
	ck_assert_int_eq(sbc.frequency, SBC_FREQ_44100);

	conf.sampling_freq_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_48000;
	/* Verify re-initialization for different sampling frequency. */
	ck_assert_int_eq(sbc_reinit_a2dp_faststream(&sbc, 0, &conf, sizeof(conf), false), 0);

	sbc_finish(&sbc);

} CK_END_TEST
#endif

CK_START_TEST(test_sbc_stderr) {
	ck_assert_str_eq(sbc_strerror(0), "Success");
	ck_assert_str_eq(sbc_strerror(-2), "Invalid sync-word");
} CK_END_TEST

int main(void) {

	Suite * s = suite_create(__FILE__);
	TCase * tc = tcase_create(__FILE__);
	SRunner * sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_sbc_a2dp_get_bitpool);
#if ENABLE_FASTSTREAM
	tcase_add_test(tc, test_sbc_init_a2dp_faststream);
#endif
	tcase_add_test(tc, test_sbc_stderr);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
