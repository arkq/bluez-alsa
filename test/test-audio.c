/*
 * test-audio.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdint.h>
#include <string.h>

#include <check.h>

#include "audio.h"
#include "shared/defs.h"

START_TEST(test_audio_scale_s16_2le) {

	const int16_t mute[] = { 0x0000, 0x0000, 0x0000, 0x0000 };
	const int16_t mute_l[] = { 0x0000, 0x2345, 0x0000, (int16_t)0xCDEF };
	const int16_t mute_r[] = { 0x1234, 0x0000, (int16_t)0xBCDE, 0x0000 };
	const int16_t half[] = { 0x1234 / 2, 0x2345 / 2, (int16_t)0xBCDE / 2, (int16_t)0xCDEF / 2 };
	const int16_t half_l[] = { 0x1234 / 2, 0x2345, (int16_t)0xBCDE / 2, (int16_t)0xCDEF };
	const int16_t half_r[] = { 0x1234, 0x2345 / 2, (int16_t)0xBCDE, (int16_t)0xCDEF / 2 };
	const int16_t in[] = { 0x1234, 0x2345, (int16_t)0xBCDE, (int16_t)0xCDEF };
	int16_t tmp[ARRAYSIZE(in)];

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s16_2le(tmp, 1, ARRAYSIZE(tmp), 0, 0);
	ck_assert_int_eq(memcmp(tmp, mute, sizeof(mute)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s16_2le(tmp, 1, ARRAYSIZE(tmp), 1.0, 0);
	ck_assert_int_eq(memcmp(tmp, in, sizeof(in)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s16_2le(tmp, 1, ARRAYSIZE(tmp), 0.5, 0);
	ck_assert_int_eq(memcmp(tmp, half, sizeof(half)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s16_2le(tmp, 2, ARRAYSIZE(tmp) / 2, 0, 1.0);
	ck_assert_int_eq(memcmp(tmp, mute_l, sizeof(mute_l)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s16_2le(tmp, 2, ARRAYSIZE(tmp) / 2, 1.0, 0);
	ck_assert_int_eq(memcmp(tmp, mute_r, sizeof(mute_r)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s16_2le(tmp, 2, ARRAYSIZE(tmp) / 2, 0.5, 1.0);
	ck_assert_int_eq(memcmp(tmp, half_l, sizeof(half_l)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s16_2le(tmp, 2, ARRAYSIZE(tmp) / 2, 1.0, 0.5);
	ck_assert_int_eq(memcmp(tmp, half_r, sizeof(half_r)), 0);

} END_TEST

START_TEST(test_audio_scale_s32_4le) {

	const int32_t mute[] = { 0, 0, 0, 0 };
	const int32_t mute_l[] = { 0, 0x23456789, 0, 0x00ABCDEF };
	const int32_t half[] = { 0x12345678 / 2, 0x23456789 / 2, 0x00123456 / 2, 0x00ABCDEF / 2 };
	const int32_t half_r[] = { 0x12345678, 0x23456789 / 2, 0x00123456, 0x00ABCDEF / 2 };
	const int32_t in[] = { 0x12345678, 0x23456789, 0x00123456, 0x00ABCDEF };
	int32_t tmp[ARRAYSIZE(in)];

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s32_4le(tmp, 1, ARRAYSIZE(tmp), 0, 0);
	ck_assert_int_eq(memcmp(tmp, mute, sizeof(mute)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s32_4le(tmp, 2, ARRAYSIZE(tmp) / 2, 0, 1.0);
	ck_assert_int_eq(memcmp(tmp, mute_l, sizeof(mute_l)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s32_4le(tmp, 1, ARRAYSIZE(tmp), 0.5, 0);
	ck_assert_int_eq(memcmp(tmp, half, sizeof(half)), 0);

	memcpy(tmp, in, sizeof(tmp));
	audio_scale_s32_4le(tmp, 2, ARRAYSIZE(tmp) / 2, 1.0, 0.5);
	ck_assert_int_eq(memcmp(tmp, half_r, sizeof(half_r)), 0);

} END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_audio_scale_s16_2le);
	tcase_add_test(tc, test_audio_scale_s32_4le);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
