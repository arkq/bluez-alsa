/*
 * test-audio.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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

#include "inc/check.inc"

CK_START_TEST(test_audio_interleave_deinterleave_s16_2le) {

	const int16_t ch1[] = { 0x0123, 0x1234, 0x2345, 0x3456 };
	const int16_t ch2[] = { 0x4567, 0x5678, 0x6789, 0x789A };

	int16_t interleaved[ARRAYSIZE(ch1) + ARRAYSIZE(ch2)];
	int16_t dest_ch1[ARRAYSIZE(ch1)];
	int16_t dest_ch2[ARRAYSIZE(ch2)];

	const int16_t *src[] = { ch1, ch2 };
	audio_interleave_s16_2le(interleaved, src, ARRAYSIZE(src), ARRAYSIZE(ch1));
	for (size_t i = 0; i < ARRAYSIZE(ch1); i++) {
		ck_assert_int_eq(interleaved[i * 2 + 0], ch1[i]);
		ck_assert_int_eq(interleaved[i * 2 + 1], ch2[i]);
	}

	int16_t *dest[] = { dest_ch1, dest_ch2 };
	audio_deinterleave_s16_2le(dest, interleaved, ARRAYSIZE(dest), ARRAYSIZE(ch1));
	ck_assert_mem_eq(dest_ch1, ch1, sizeof(ch1));
	ck_assert_mem_eq(dest_ch2, ch2, sizeof(ch2));

} CK_END_TEST

CK_START_TEST(test_audio_interleave_deinterleave_s32_4le) {

	const int32_t ch1[] = { 0x01234567, 0x12345678, 0x23456789, 0x3456789A };
	const int32_t ch2[] = { 0x456789AB, 0x56789ABC, 0x6789ABCD, 0x789ABCDE };
	const int32_t ch3[] = { 0x89ABCDEF, 0x9ABCDEF0, 0xABCDEF01, 0xBCDEF012 };

	int32_t interleaved[ARRAYSIZE(ch1) + ARRAYSIZE(ch2) + ARRAYSIZE(ch3)];
	int32_t dest_ch1[ARRAYSIZE(ch1)];
	int32_t dest_ch2[ARRAYSIZE(ch2)];
	int32_t dest_ch3[ARRAYSIZE(ch3)];

	const int32_t *src[] = { ch1, ch2, ch3 };
	audio_interleave_s32_4le(interleaved, src, ARRAYSIZE(src), ARRAYSIZE(ch1));
	for (size_t i = 0; i < ARRAYSIZE(ch1); i++) {
		ck_assert_int_eq(interleaved[i * 3 + 0], ch1[i]);
		ck_assert_int_eq(interleaved[i * 3 + 1], ch2[i]);
		ck_assert_int_eq(interleaved[i * 3 + 2], ch3[i]);
	}

	int32_t *dest[] = { dest_ch1, dest_ch2, dest_ch3 };
	audio_deinterleave_s32_4le(dest, interleaved, ARRAYSIZE(dest), ARRAYSIZE(ch1));
	ck_assert_mem_eq(dest_ch1, ch1, sizeof(ch1));
	ck_assert_mem_eq(dest_ch2, ch2, sizeof(ch2));
	ck_assert_mem_eq(dest_ch3, ch3, sizeof(ch3));

} CK_END_TEST

CK_START_TEST(test_audio_scale_s16_2le) {

	const int16_t mute[] = { 0x0000, 0x0000, 0x0000, 0x0000 };
	const int16_t mute_l[] = { 0x0000, 0x2345, 0x0000, (int16_t)0xCDEF };
	const int16_t mute_r[] = { 0x1234, 0x0000, (int16_t)0xBCDE, 0x0000 };
	const int16_t half[] = { 0x1234 / 2, 0x2345 / 2, (int16_t)0xBCDE / 2, (int16_t)0xCDEF / 2 };
	const int16_t half_l[] = { 0x1234 / 2, 0x2345, (int16_t)0xBCDE / 2, (int16_t)0xCDEF };
	const int16_t half_r[] = { 0x1234, 0x2345 / 2, (int16_t)0xBCDE, (int16_t)0xCDEF / 2 };
	const int16_t in[] = { 0x1234, 0x2345, (int16_t)0xBCDE, (int16_t)0xCDEF };
	int16_t tmp[ARRAYSIZE(in)];

	memcpy(tmp, in, sizeof(tmp));
	const double scale_mute[] = { 0.0 };
	audio_scale_s16_2le(tmp, scale_mute, 1, ARRAYSIZE(tmp));
	ck_assert_mem_eq(tmp, mute, sizeof(mute));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_none[] = { 1.0 };
	audio_scale_s16_2le(tmp, scale_none, 1, ARRAYSIZE(tmp));
	ck_assert_mem_eq(tmp, in, sizeof(in));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_half[] = { 0.5 };
	audio_scale_s16_2le(tmp, scale_half, 1, ARRAYSIZE(tmp));
	ck_assert_mem_eq(tmp, half, sizeof(half));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_mute_l[] = { 0.0, 1.0 };
	audio_scale_s16_2le(tmp, scale_mute_l, 2, ARRAYSIZE(tmp) / 2);
	ck_assert_mem_eq(tmp, mute_l, sizeof(mute_l));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_mute_r[] = { 1.0, 0.0 };
	audio_scale_s16_2le(tmp, scale_mute_r, 2, ARRAYSIZE(tmp) / 2);
	ck_assert_mem_eq(tmp, mute_r, sizeof(mute_r));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_half_l[] = { 0.5, 1.0 };
	audio_scale_s16_2le(tmp, scale_half_l, 2, ARRAYSIZE(tmp) / 2);
	ck_assert_mem_eq(tmp, half_l, sizeof(half_l));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_half_r[] = { 1.0, 0.5 };
	audio_scale_s16_2le(tmp, scale_half_r, 2, ARRAYSIZE(tmp) / 2);
	ck_assert_mem_eq(tmp, half_r, sizeof(half_r));

} CK_END_TEST

CK_START_TEST(test_audio_scale_s32_4le) {

	const int32_t mute[] = { 0, 0, 0, 0 };
	const int32_t mute_l[] = { 0, 0x23456789, 0, 0x00ABCDEF };
	const int32_t half[] = { 0x12345678 / 2, 0x23456789 / 2, 0x00123456 / 2, 0x00ABCDEF / 2 };
	const int32_t half_r[] = { 0x12345678, 0x23456789 / 2, 0x00123456, 0x00ABCDEF / 2 };
	const int32_t in[] = { 0x12345678, 0x23456789, 0x00123456, 0x00ABCDEF };
	int32_t tmp[ARRAYSIZE(in)];

	memcpy(tmp, in, sizeof(tmp));
	const double scale_mute[] = { 0.0 };
	audio_scale_s32_4le(tmp, scale_mute, 1, ARRAYSIZE(tmp));
	ck_assert_mem_eq(tmp, mute, sizeof(mute));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_mute_l[] = { 0.0, 1.0 };
	audio_scale_s32_4le(tmp, scale_mute_l, 2, ARRAYSIZE(tmp) / 2);
	ck_assert_mem_eq(tmp, mute_l, sizeof(mute_l));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_half[] = { 0.5 };
	audio_scale_s32_4le(tmp, scale_half, 1, ARRAYSIZE(tmp));
	ck_assert_mem_eq(tmp, half, sizeof(half));

	memcpy(tmp, in, sizeof(tmp));
	const double scale_half_r[] = { 1.0, 0.5 };
	audio_scale_s32_4le(tmp, scale_half_r, 2, ARRAYSIZE(tmp) / 2);
	ck_assert_mem_eq(tmp, half_r, sizeof(half_r));

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_audio_interleave_deinterleave_s16_2le);
	tcase_add_test(tc, test_audio_interleave_deinterleave_s32_4le);
	tcase_add_test(tc, test_audio_scale_s16_2le);
	tcase_add_test(tc, test_audio_scale_s32_4le);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
