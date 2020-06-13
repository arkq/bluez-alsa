/*
 * test-a2dp.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <check.h>

#include "../src/a2dp.c"
#include "../src/bluealsa.c"
#include "../src/shared/log.c"

START_TEST(test_a2dp_dir) {
	ck_assert_int_eq(A2DP_SOURCE, !A2DP_SINK);
	ck_assert_int_eq(!A2DP_SOURCE, A2DP_SINK);
} END_TEST

START_TEST(test_a2dp_codec_lookup) {
	ck_assert_ptr_eq(a2dp_codec_lookup(A2DP_CODEC_SBC, A2DP_SOURCE), &a2dp_codec_source_sbc);
	ck_assert_ptr_eq(a2dp_codec_lookup(0xFFFF, A2DP_SOURCE), NULL);
} END_TEST

START_TEST(test_a2dp_get_vendor_codec_id) {

	uint8_t cfg0[4] = { 0xDE, 0xAD, 0xB0, 0xBE };
	ck_assert_int_eq(a2dp_get_vendor_codec_id(cfg0, sizeof(cfg0)), 0xFFFF);
	ck_assert_int_eq(errno, EINVAL);

	a2dp_aptx_t cfg1 = { A2DP_SET_VENDOR_ID_CODEC_ID(APTX_VENDOR_ID, APTX_CODEC_ID), 0, 0 };
	ck_assert_int_eq(a2dp_get_vendor_codec_id(&cfg1, sizeof(cfg1)), A2DP_CODEC_VENDOR_APTX);

	a2dp_aptx_t cfg2 = { A2DP_SET_VENDOR_ID_CODEC_ID(APTX_VENDOR_ID, 0x69), 0, 0 };
	ck_assert_int_eq(a2dp_get_vendor_codec_id(&cfg2, sizeof(cfg2)), 0xFFFF);
	ck_assert_int_eq(errno, ENOTSUP);

} END_TEST

START_TEST(test_a2dp_filter_capabilities) {

	a2dp_sbc_t cfg = {
		.frequency = SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_STEREO,
		.block_length = SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8,
		.subbands = SBC_SUBBANDS_4,
		.allocation_method = SBC_ALLOCATION_SNR,
		.min_bitpool = 42,
		.max_bitpool = 255,
	};

	hexdump("Capabilities original", &cfg, sizeof(cfg));
	ck_assert_int_eq(a2dp_filter_capabilities(&a2dp_codec_source_sbc, &cfg, sizeof(cfg)), 0);

	hexdump("Capabilities filtered", &cfg, sizeof(cfg));
	ck_assert_int_eq(cfg.frequency, SBC_SAMPLING_FREQ_44100);
	ck_assert_int_eq(cfg.channel_mode, SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_STEREO);
	ck_assert_int_eq(cfg.block_length, SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8);
	ck_assert_int_eq(cfg.subbands, SBC_SUBBANDS_4);
	ck_assert_int_eq(cfg.allocation_method, SBC_ALLOCATION_SNR);
	ck_assert_int_eq(cfg.min_bitpool, MAX(SBC_MIN_BITPOOL, 42));
	ck_assert_int_eq(cfg.max_bitpool, MIN(SBC_MAX_BITPOOL, 255));

} END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_a2dp_dir);
	tcase_add_test(tc, test_a2dp_codec_lookup);
	tcase_add_test(tc, test_a2dp_get_vendor_codec_id);
	tcase_add_test(tc, test_a2dp_filter_capabilities);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
