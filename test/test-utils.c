/*
 * test-utils.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <bluetooth/bluetooth.h>
#include <check.h>

#include "hci.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/hex.h"
#include "shared/nv.h"
#include "shared/rt.h"

#include "inc/check.inc"

CK_START_TEST(test_g_dbus_bluez_object_path_to_hci_dev_id) {

	ck_assert_int_eq(g_dbus_bluez_object_path_to_hci_dev_id("/org/bluez"), -1);
	ck_assert_int_eq(g_dbus_bluez_object_path_to_hci_dev_id("/org/bluez/hci0"), 0);
	ck_assert_int_eq(g_dbus_bluez_object_path_to_hci_dev_id("/org/bluez/hci5"), 5);

} CK_END_TEST

CK_START_TEST(test_g_dbus_bluez_object_path_to_bdaddr) {

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

} CK_END_TEST

CK_START_TEST(test_g_variant_sanitize_object_path) {

	char path1[] = "/some/valid_path/123";
	char path2[] = "/a#$*/invalid-path";

	ck_assert_str_eq(g_variant_sanitize_object_path(path1), "/some/valid_path/123");
	ck_assert_str_eq(g_variant_sanitize_object_path(path2), "/a___/invalid_path");

} CK_END_TEST

#if DEBUG
CK_START_TEST(test_batostr_) {

	const bdaddr_t ba = {{ 1, 2, 3, 4, 5, 6 }};
	char tmp[18];

	ba2str(&ba, tmp);
	ck_assert_str_eq(batostr_(&ba), tmp);

} CK_END_TEST
#endif

CK_START_TEST(test_nv_find) {

	const nv_entry_t entries[] = {
		{ "name1", .v.i = 1 },
		{ "name2", .v.i = 2 },
		{ 0 }
	};

	ck_assert_ptr_eq(nv_find(entries, "invalid"), NULL);
	ck_assert_ptr_eq(nv_find(entries, "name2"), &entries[1]);

} CK_END_TEST

CK_START_TEST(test_nv_join_names) {

	const nv_entry_t entries_zero[] = {{ 0 }};
	const nv_entry_t entries[] = {
		{ "name1", .v.i = 1 },
		{ "name2", .v.i = 2 },
		{ 0 }
	};

	char *tmp;

	ck_assert_str_eq(tmp = nv_join_names(entries_zero), "");
	free(tmp);

	ck_assert_str_eq(tmp = nv_join_names(entries), "name1, name2");
	free(tmp);

} CK_END_TEST

CK_START_TEST(test_difftimespec) {

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

} CK_END_TEST

CK_START_TEST(test_ffb) {

	ffb_t ffb_u8 = { 0 };
	ffb_t ffb_16 = { 0 };

	/* allow free before allocation */
	ffb_free(&ffb_u8);
	ffb_free(&ffb_16);

	ck_assert_int_eq(ffb_init_uint8_t(&ffb_u8, 64), 0);
	ck_assert_ptr_eq(ffb_u8.data, ffb_u8.tail);
	ck_assert_int_eq(ffb_u8.nmemb, 64);

	ck_assert_int_eq(ffb_init_int16_t(&ffb_16, 64), 0);
	ck_assert_ptr_eq(ffb_16.data, ffb_16.tail);
	ck_assert_int_eq(ffb_16.nmemb, 64);

	memcpy(ffb_u8.data, "1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ", 36);
	ffb_seek(&ffb_u8, 36);

	memcpy(ffb_16.data, "11223344556677889900AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVVWWXXYYZZ", 36 * 2);
	ffb_seek(&ffb_16, 36);

	ck_assert_int_eq(ffb_len_in(&ffb_u8), 64 - 36);
	ck_assert_int_eq(ffb_blen_in(&ffb_u8), 64 - 36);
	ck_assert_int_eq(ffb_len_out(&ffb_u8), 36);
	ck_assert_int_eq(ffb_blen_out(&ffb_u8), 36);
	ck_assert_int_eq(((uint8_t *)ffb_u8.tail)[-1], 'Z');

	ck_assert_int_eq(ffb_len_in(&ffb_16), 64 - 36);
	ck_assert_int_eq(ffb_blen_in(&ffb_16), (64 - 36) * 2);
	ck_assert_int_eq(ffb_len_out(&ffb_16), 36);
	ck_assert_int_eq(ffb_blen_out(&ffb_16), 36 * 2);
	ck_assert_int_eq(((int16_t *)ffb_16.tail)[-1], 0x5a5a);

	ck_assert_int_eq(ffb_shift(&ffb_u8, 33), 33);
	ck_assert_int_eq(ffb_len_in(&ffb_u8), 64 - (36 - 33));
	ck_assert_int_eq(ffb_len_out(&ffb_u8), 36 - 33);
	ck_assert_mem_eq(ffb_u8.data, "XYZ", ffb_len_out(&ffb_u8));
	ck_assert_int_eq(((uint8_t *)ffb_u8.tail)[-1], 'Z');

	ck_assert_int_eq(ffb_shift(&ffb_u8, 100), 36 - 33);
	ck_assert_ptr_eq(ffb_u8.data, ffb_u8.tail);

	ffb_seek(&ffb_u8, 4);
	ck_assert_ptr_ne(ffb_u8.data, ffb_u8.tail);

	ffb_rewind(&ffb_u8);
	ck_assert_ptr_eq(ffb_u8.data, ffb_u8.tail);

	ffb_free(&ffb_u8);
	ck_assert_ptr_eq(ffb_u8.data, NULL);

	ffb_free(&ffb_16);
	ck_assert_ptr_eq(ffb_16.data, NULL);

} CK_END_TEST

CK_START_TEST(test_ffb_static) {

	ffb_t ffb = { 0 };
	uint32_t buffer[64];

	ffb_init_from_array(&ffb, buffer);

	ck_assert_ptr_eq(ffb.data, buffer);
	ck_assert_ptr_eq(ffb.tail, buffer);
	ck_assert_uint_eq(ffb.nmemb, ARRAYSIZE(buffer));
	ck_assert_uint_eq(ffb.size, 4);

} CK_END_TEST

CK_START_TEST(test_ffb_resize) {

	const char *data = "1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	const size_t data_len = strlen(data);

	ffb_t ffb = { 0 };
	ck_assert_int_eq(ffb_init_uint8_t(&ffb, 64), 0);

	memcpy(ffb.data, data, data_len);
	ffb_seek(&ffb, data_len);

	ck_assert_int_eq(ffb_len_out(&ffb), data_len);
	ck_assert_int_eq(ffb_len_in(&ffb), 64 - data_len);
	ck_assert_mem_eq(ffb.data, data, data_len);

	ck_assert_int_eq(ffb_init_uint8_t(&ffb, 128), 0);

	ck_assert_int_eq(ffb_len_out(&ffb), data_len);
	ck_assert_int_eq(ffb_len_in(&ffb), 128 - data_len);
	ck_assert_mem_eq(ffb.data, data, data_len);

	ffb_free(&ffb);

} CK_END_TEST

CK_START_TEST(test_bin2hex) {

	const uint8_t bin[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF };
	char hex[sizeof(bin) * 2 + 1];

	ck_assert_int_eq(bin2hex(bin, hex, sizeof(bin)), 12);
	ck_assert_str_eq(hex, "deadbeef00ff");

} CK_END_TEST

CK_START_TEST(test_hex2bin) {

	const uint8_t bin_ok[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00 };
	const char hex[] = "DEADbeef\x00\xFF";
	uint8_t bin[sizeof(bin_ok)];

	ck_assert_int_eq(hex2bin(hex, bin, sizeof(hex) - 1), 5);
	ck_assert_mem_eq(bin, bin_ok, sizeof(bin));

	ck_assert_int_eq(hex2bin(hex, bin, 3), -1);
	ck_assert_int_eq(errno, EINVAL);

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	/* shared/ffb.c */
	tcase_add_test(tc, test_ffb);
	tcase_add_test(tc, test_ffb_static);
	tcase_add_test(tc, test_ffb_resize);

	/* shared/hex.c */
	tcase_add_test(tc, test_bin2hex);
	tcase_add_test(tc, test_hex2bin);

	/* shared/nv.c */
	tcase_add_test(tc, test_nv_find);
	tcase_add_test(tc, test_nv_join_names);

	/* shared/rt.c */
	tcase_add_test(tc, test_difftimespec);

	tcase_add_test(tc, test_g_dbus_bluez_object_path_to_hci_dev_id);
	tcase_add_test(tc, test_g_dbus_bluez_object_path_to_bdaddr);
	tcase_add_test(tc, test_g_variant_sanitize_object_path);
#if DEBUG
	tcase_add_test(tc, test_batostr_);
#endif

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
