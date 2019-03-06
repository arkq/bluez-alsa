/*
 * test-utils.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <check.h>

#include "../src/utils.c"
#include "../src/shared/defs.h"
#include "../src/shared/ffb.c"
#include "../src/shared/log.c"
#include "../src/shared/rt.c"

START_TEST(test_g_dbus_bluez_object_path_to_hci_dev_id) {

	ck_assert_int_eq(g_dbus_bluez_object_path_to_hci_dev_id("/org/bluez"), -1);
	ck_assert_int_eq(g_dbus_bluez_object_path_to_hci_dev_id("/org/bluez/hci0"), 0);
	ck_assert_int_eq(g_dbus_bluez_object_path_to_hci_dev_id("/org/bluez/hci5"), 5);

} END_TEST

START_TEST(test_g_dbus_bluez_object_path_to_bdaddr) {

	bdaddr_t addr_ok = {{ 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12 }};
	bdaddr_t addr;

	ck_assert_ptr_eq(g_dbus_bluez_object_path_to_bdaddr(
				"/org/bluez/hci0/dev_12_34_56_78_9A_BC", &addr), &addr);
	ck_assert_int_eq(bacmp(&addr, &addr_ok), 0);

	ck_assert_ptr_eq(g_dbus_bluez_object_path_to_bdaddr(
				"/org/bluez/dev_12_34_56_78_9A_BC/fd1", &addr), &addr);
	ck_assert_int_eq(bacmp(&addr, &addr_ok), 0);

	ck_assert_ptr_eq(g_dbus_bluez_object_path_to_bdaddr(
				"/org/bluez/dev_12_34_56_78_9A_XX", &addr), NULL);

} END_TEST

START_TEST(test_dbus_profile_object_path) {

	static const struct {
		struct ba_transport_type ttype;
		const char *path;
	} profiles[] = {
		/* test null/invalid path */
		{ { 0, -1 }, "/" },
		{ { 0, -1 }, "/Invalid" },
		/* test A2DP profiles */
		{ { BA_TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC }, "/A2DP/SBC/Source" },
		{ { BA_TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC }, "/A2DP/SBC/Source/1" },
		{ { BA_TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC }, "/A2DP/SBC/Source/2" },
		{ { BA_TRANSPORT_PROFILE_A2DP_SINK, A2DP_CODEC_SBC }, "/A2DP/SBC/Sink" },
#if ENABLE_MPEG
		{ { BA_TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_MPEG12 }, "/A2DP/MPEG12/Source" },
		{ { BA_TRANSPORT_PROFILE_A2DP_SINK, A2DP_CODEC_MPEG12 }, "/A2DP/MPEG12/Sink" },
#endif
#if ENABLE_AAC
		{ { BA_TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_MPEG24 }, "/A2DP/MPEG24/Source" },
		{ { BA_TRANSPORT_PROFILE_A2DP_SINK, A2DP_CODEC_MPEG24 }, "/A2DP/MPEG24/Sink" },
#endif
#if ENABLE_APTX
		{ { BA_TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_VENDOR_APTX }, "/A2DP/APTX/Source" },
		{ { BA_TRANSPORT_PROFILE_A2DP_SINK, A2DP_CODEC_VENDOR_APTX }, "/A2DP/APTX/Sink" },
#endif
#if ENABLE_APTX_HD
		{ { BA_TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_VENDOR_APTX_HD }, "/A2DP/APTXHD/Source" },
		{ { BA_TRANSPORT_PROFILE_A2DP_SINK, A2DP_CODEC_VENDOR_APTX_HD }, "/A2DP/APTXHD/Sink" },
#endif
#if ENABLE_LDAC
		{ { BA_TRANSPORT_PROFILE_A2DP_SOURCE, A2DP_CODEC_VENDOR_LDAC }, "/A2DP/LDAC/Source" },
		{ { BA_TRANSPORT_PROFILE_A2DP_SINK, A2DP_CODEC_VENDOR_LDAC }, "/A2DP/LDAC/Sink" },
#endif
		/* test HSP/HFP profiles */
		{ { BA_TRANSPORT_PROFILE_HSP_HS, HFP_CODEC_CVSD }, "/HSP/Headset" },
		{ { BA_TRANSPORT_PROFILE_HSP_AG, HFP_CODEC_CVSD }, "/HSP/AudioGateway" },
		{ { BA_TRANSPORT_PROFILE_HFP_HF, HFP_CODEC_UNDEFINED }, "/HFP/HandsFree" },
		{ { BA_TRANSPORT_PROFILE_HFP_AG, HFP_CODEC_UNDEFINED }, "/HFP/AudioGateway" },
	};

	size_t i;

	for (i = 0; i < ARRAYSIZE(profiles); i++) {
		const char *path = g_dbus_transport_type_to_bluez_object_path(profiles[i].ttype);
		struct ba_transport_type ttype = g_dbus_bluez_object_path_to_transport_type(profiles[i].path);
		ck_assert_str_eq(strstr(profiles[i].path, path), profiles[i].path);
		ck_assert_int_eq(ttype.profile, profiles[i].ttype.profile);
		ck_assert_int_eq(ttype.codec, profiles[i].ttype.codec);
	}

} END_TEST

START_TEST(test_batostr_) {

	const bdaddr_t ba = {{ 1, 2, 3, 4, 5, 6 }};
	char tmp[18];

	ba2str(&ba, tmp);
	ck_assert_str_eq(batostr_(&ba), tmp);

} END_TEST

START_TEST(test_snd_pcm_scale_s16le) {

	const int16_t mute[] = { 0x0000, 0x0000, 0x0000, 0x0000 };
	const int16_t half[] = { 0x1234 / 2, 0x2345 / 2, (int16_t)0xBCDE / 2, (int16_t)0xCDEF / 2 };
	const int16_t halfl[] = { 0x1234 / 2, 0x2345, (int16_t)0xBCDE / 2, (int16_t)0xCDEF };
	const int16_t halfr[] = { 0x1234, 0x2345 / 2, (int16_t)0xBCDE, (int16_t)0xCDEF / 2 };
	const int16_t in[] = { 0x1234, 0x2345, (int16_t)0xBCDE, (int16_t)0xCDEF };
	int16_t tmp[ARRAYSIZE(in)];

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, ARRAYSIZE(tmp), 1, 0, 0);
	ck_assert_int_eq(memcmp(tmp, mute, sizeof(mute)), 0);

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, ARRAYSIZE(tmp), 1, 1.0, 1.0);
	ck_assert_int_eq(memcmp(tmp, in, sizeof(in)), 0);

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, ARRAYSIZE(tmp), 1, 0.5, 0.5);
	ck_assert_int_eq(memcmp(tmp, half, sizeof(half)), 0);

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, ARRAYSIZE(tmp), 2, 0.5, 1.0);
	ck_assert_int_eq(memcmp(tmp, halfl, sizeof(halfl)), 0);

	memcpy(tmp, in, sizeof(tmp));
	snd_pcm_scale_s16le(tmp, ARRAYSIZE(tmp), 2, 1.0, 0.5);
	ck_assert_int_eq(memcmp(tmp, halfr, sizeof(halfr)), 0);

} END_TEST

START_TEST(test_difftimespec) {

	struct timespec ts1, ts2, ts;

	ts1.tv_sec = ts2.tv_sec = 12345;
	ts1.tv_nsec = ts2.tv_nsec = 67890;
	ck_assert_int_eq(difftimespec(&ts1, &ts2, &ts), 0);
	ck_assert_int_eq(ts.tv_sec, 0);
	ck_assert_int_eq(ts.tv_nsec, 0);

	ts1.tv_sec = 10;
	ts1.tv_nsec = 100000000;
	ts2.tv_sec = 10;
	ts2.tv_nsec = 500000000;
	ck_assert_int_gt(difftimespec(&ts1, &ts2, &ts), 0);
	ck_assert_int_eq(ts.tv_sec, 0);
	ck_assert_int_eq(ts.tv_nsec, 400000000);

	ts1.tv_sec = 10;
	ts1.tv_nsec = 100000000;
	ts2.tv_sec = 11;
	ts2.tv_nsec = 500000000;
	ck_assert_int_gt(difftimespec(&ts1, &ts2, &ts), 0);
	ck_assert_int_eq(ts.tv_sec, 1);
	ck_assert_int_eq(ts.tv_nsec, 400000000);

	ts1.tv_sec = 10;
	ts1.tv_nsec = 800000000;
	ts2.tv_sec = 12;
	ts2.tv_nsec = 100000000;
	ck_assert_int_gt(difftimespec(&ts1, &ts2, &ts), 0);
	ck_assert_int_eq(ts.tv_sec, 1);
	ck_assert_int_eq(ts.tv_nsec, 300000000);

	ts1.tv_sec = 10;
	ts1.tv_nsec = 500000000;
	ts2.tv_sec = 10;
	ts2.tv_nsec = 100000000;
	ck_assert_int_lt(difftimespec(&ts1, &ts2, &ts), 0);
	ck_assert_int_eq(ts.tv_sec, 0);
	ck_assert_int_eq(ts.tv_nsec, 400000000);

	ts1.tv_sec = 12;
	ts1.tv_nsec = 100000000;
	ts2.tv_sec = 10;
	ts2.tv_nsec = 800000000;
	ck_assert_int_lt(difftimespec(&ts1, &ts2, &ts), 0);
	ck_assert_int_eq(ts.tv_sec, 1);
	ck_assert_int_eq(ts.tv_nsec, 300000000);

	ts1.tv_sec = 12;
	ts1.tv_nsec = 500000000;
	ts2.tv_sec = 10;
	ts2.tv_nsec = 500000000;
	ck_assert_int_lt(difftimespec(&ts1, &ts2, &ts), 0);
	ck_assert_int_eq(ts.tv_sec, 2);
	ck_assert_int_eq(ts.tv_nsec, 0);

} END_TEST

START_TEST(test_fifo_buffer) {

	ffb_uint8_t ffb_u8 = { 0 };
	ffb_int16_t ffb_16 = { 0 };

	/* allow free before allocation */
	ffb_uint8_free(&ffb_u8);
	ffb_int16_free(&ffb_16);

	ck_assert_ptr_ne(ffb_init(&ffb_u8, 64), NULL);
	ck_assert_ptr_eq(ffb_u8.data, ffb_u8.tail);
	ck_assert_int_eq(ffb_u8.size, 64);

	ck_assert_ptr_ne(ffb_init(&ffb_16, 64), NULL);
	ck_assert_ptr_eq(ffb_16.data, ffb_16.tail);
	ck_assert_int_eq(ffb_16.size, 64);

	memcpy(ffb_u8.data, "1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ", 36);
	ffb_seek(&ffb_u8, 36);

	memcpy(ffb_16.data, "11223344556677889900AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVVWWXXYYZZ", 36 * 2);
	ffb_seek(&ffb_16, 36);

	ck_assert_int_eq(ffb_len_in(&ffb_u8), 64 - 36);
	ck_assert_int_eq(ffb_blen_in(&ffb_u8), 64 - 36);
	ck_assert_int_eq(ffb_len_out(&ffb_u8), 36);
	ck_assert_int_eq(ffb_blen_out(&ffb_u8), 36);
	ck_assert_int_eq(ffb_u8.tail[-1], 'Z');

	ck_assert_int_eq(ffb_len_in(&ffb_16), 64 - 36);
	ck_assert_int_eq(ffb_blen_in(&ffb_16), (64 - 36) * 2);
	ck_assert_int_eq(ffb_len_out(&ffb_16), 36);
	ck_assert_int_eq(ffb_blen_out(&ffb_16), 36 * 2);
	ck_assert_int_eq(ffb_16.tail[-1], 0x5a5a);

	ffb_shift(&ffb_u8, 15);
	ck_assert_int_eq(ffb_len_in(&ffb_u8), 64 - (36 - 15));
	ck_assert_int_eq(ffb_len_out(&ffb_u8), 36 - 15);
	ck_assert_int_eq(memcmp(ffb_u8.data, "FGHIJKLMNOPQRSTUVWXYZ", ffb_len_out(&ffb_u8)), 0);
	ck_assert_int_eq(ffb_u8.tail[-1], 'Z');

	ffb_rewind(&ffb_u8);
	ck_assert_ptr_eq(ffb_u8.data, ffb_u8.tail);

	ffb_uint8_free(&ffb_u8);
	ck_assert_ptr_eq(ffb_u8.data, NULL);

	ffb_int16_free(&ffb_16);
	ck_assert_ptr_eq(ffb_16.data, NULL);

} END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_g_dbus_bluez_object_path_to_hci_dev_id);
	tcase_add_test(tc, test_g_dbus_bluez_object_path_to_bdaddr);
	tcase_add_test(tc, test_dbus_profile_object_path);
	tcase_add_test(tc, test_batostr_);
	tcase_add_test(tc, test_snd_pcm_scale_s16le);
	tcase_add_test(tc, test_difftimespec);
	tcase_add_test(tc, test_fifo_buffer);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
