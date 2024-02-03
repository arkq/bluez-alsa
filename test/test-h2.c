/*
 * test-h2.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdint.h>
#include <stdio.h>

#include <check.h>

#include "h2.h"
#include "shared/defs.h"

#include "inc/check.inc"

CK_START_TEST(test_h2_header_pack) {

	static const h2_header_t h2_0 = HTOLE16(0x0801);
	static const h2_header_t h2_1 = HTOLE16(0x3801);
	static const h2_header_t h2_2 = HTOLE16(0xC801);
	static const h2_header_t h2_3 = HTOLE16(0xF801);

	ck_assert_uint_eq(h2_header_pack(0), h2_0);
	ck_assert_uint_eq(h2_header_pack(1), h2_1);
	ck_assert_uint_eq(h2_header_pack(2), h2_2);
	ck_assert_uint_eq(h2_header_pack(3), h2_3);

} CK_END_TEST

CK_START_TEST(test_h2_header_unpack) {

	static const h2_header_t h2_0 = HTOLE16(0x0801);
	static const h2_header_t h2_1 = HTOLE16(0x3801);
	static const h2_header_t h2_2 = HTOLE16(0xC801);
	static const h2_header_t h2_3 = HTOLE16(0xF801);

	ck_assert_uint_eq(h2_header_unpack(h2_0), 0);
	ck_assert_uint_eq(h2_header_unpack(h2_1), 1);
	ck_assert_uint_eq(h2_header_unpack(h2_2), 2);
	ck_assert_uint_eq(h2_header_unpack(h2_3), 3);

} CK_END_TEST

CK_START_TEST(test_h2_header_find) {

	static const uint8_t raw[][10] = {
		{ 0 },
		/* H2 header starts at first byte */
		{ 0x01, 0x08, 0xad, 0x00, 0x00, 0xd5, 0x10, 0x00, 0x11, 0x10 },
		/* H2 header starts at 5th byte */
		{ 0x00, 0xd5, 0x10, 0x00, 0x01, 0x38, 0xad, 0x00, 0x11, 0x10 },
		/* first H2 header starts at 2nd byte (second at 6th byte) */
		{ 0xd5, 0x01, 0xc8, 0xad, 0x00, 0x01, 0xf8, 0xad, 0x11, 0x10 },
		/* incorrect sequence number (bit not duplicated) */
		{ 0x01, 0x18, 0xad, 0x00, 0x00, 0xd5, 0x10, 0x00, 0x11, 0x10 },
		{ 0x01, 0x58, 0xad, 0x00, 0x00, 0xd5, 0x10, 0x00, 0x11, 0x10 },
	};

	size_t len;

	len = sizeof(*raw);
	ck_assert_ptr_eq(h2_header_find(raw[0], &len), NULL);
	ck_assert_int_eq(len, 1);

	len = sizeof(*raw);
	ck_assert_ptr_eq(h2_header_find(raw[1], &len), (h2_header_t *)&raw[1][0]);
	ck_assert_int_eq(len, sizeof(*raw) - 0);

	len = sizeof(*raw);
	ck_assert_ptr_eq(h2_header_find(raw[2], &len), (h2_header_t *)&raw[2][4]);
	ck_assert_int_eq(len, sizeof(*raw) - 4);

	len = sizeof(*raw);
	ck_assert_ptr_eq(h2_header_find(raw[3], &len), (h2_header_t *)&raw[3][1]);
	ck_assert_int_eq(len, sizeof(*raw) - 1);

	len = sizeof(*raw);
	ck_assert_ptr_eq(h2_header_find(raw[4], &len), NULL);
	ck_assert_int_eq(len, 1);

	len = sizeof(*raw);
	ck_assert_ptr_eq(h2_header_find(raw[5], &len), NULL);
	ck_assert_int_eq(len, 1);

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_h2_header_pack);
	tcase_add_test(tc, test_h2_header_unpack);
	tcase_add_test(tc, test_h2_header_find);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
