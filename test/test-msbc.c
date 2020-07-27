/*
 * test-msbc.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <check.h>

#include "inc/sine.inc"
#include "../src/msbc.c"
#include "../src/shared/defs.h"
#include "../src/shared/ffb.c"
#include "../src/shared/log.c"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

START_TEST(test_msbc_init) {

	struct esco_msbc msbc = { .initialized = false };

	ck_assert_int_eq(msbc_init(&msbc), 0);
	ck_assert_int_eq(msbc.initialized, true);
	ck_assert_int_eq(ffb_len_out(&msbc.enc_pcm), 0);

	ffb_seek(&msbc.enc_pcm, 16);
	ck_assert_int_eq(ffb_len_out(&msbc.enc_pcm), 16);

	ck_assert_int_eq(msbc_init(&msbc), 0);
	ck_assert_int_eq(msbc.initialized, true);
	ck_assert_int_eq(ffb_len_out(&msbc.enc_pcm), 0);

	msbc_finish(&msbc);

} END_TEST

START_TEST(test_msbc_find_h2_header) {

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
	ck_assert_ptr_eq(msbc_find_h2_header(raw[0], &len), NULL);
	ck_assert_int_eq(len, 1);

	len = sizeof(*raw);
	ck_assert_ptr_eq(msbc_find_h2_header(raw[1], &len), (esco_h2_header_t *)&raw[1][0]);
	ck_assert_int_eq(len, sizeof(*raw) - 0);

	len = sizeof(*raw);
	ck_assert_ptr_eq(msbc_find_h2_header(raw[2], &len), (esco_h2_header_t *)&raw[2][4]);
	ck_assert_int_eq(len, sizeof(*raw) - 4);

	len = sizeof(*raw);
	ck_assert_ptr_eq(msbc_find_h2_header(raw[3], &len), (esco_h2_header_t *)&raw[3][1]);
	ck_assert_int_eq(len, sizeof(*raw) - 1);

	len = sizeof(*raw);
	ck_assert_ptr_eq(msbc_find_h2_header(raw[4], &len), NULL);
	ck_assert_int_eq(len, 1);

	len = sizeof(*raw);
	ck_assert_ptr_eq(msbc_find_h2_header(raw[5], &len), NULL);
	ck_assert_int_eq(len, 1);

} END_TEST

START_TEST(test_msbc_encode_decode) {

	struct esco_msbc msbc = { .initialized = false };
	int16_t sine[1024];
	size_t len;
	size_t i;
	int rv;

	ck_assert_int_eq(msbc_init(&msbc), 0);
	snd_pcm_sine_s16le(sine, sizeof(sine) / sizeof(int16_t), 1, 0, 0.01);

	uint8_t data[sizeof(sine)];
	uint8_t *data_tail = data;

	for (rv = 1, i = 0; rv == 1;) {

		len = MIN(ARRAYSIZE(sine) - i, ffb_len_in(&msbc.enc_pcm));
		memcpy(msbc.enc_pcm.tail, &sine[i], len * sizeof(int16_t));
		ffb_seek(&msbc.enc_pcm, len);
		i += len;

		rv = msbc_encode(&msbc);

		len = ffb_blen_out(&msbc.enc_data);
		memcpy(data_tail, msbc.enc_data.data, len);
		ffb_shift(&msbc.enc_data, len);
		data_tail += len;

	}

	ck_assert_int_eq(data_tail - data, 480);

	int16_t pcm[sizeof(sine)];
	int16_t *pcm_tail = pcm;

	for (rv = 1, i = 0; rv == 1; ) {

		len = MIN((data_tail - data) - i, ffb_blen_in(&msbc.dec_data));
		memcpy(msbc.dec_data.tail, &data[i], len);
		ffb_seek(&msbc.dec_data, len);
		i += len;

		rv = msbc_decode(&msbc);

		len = ffb_len_out(&msbc.dec_pcm);
		memcpy(pcm_tail, msbc.dec_pcm.data, len * sizeof(int16_t));
		ffb_shift(&msbc.dec_pcm, len);
		pcm_tail += len;

	}

	ck_assert_int_eq(pcm_tail - pcm, 960);

	msbc_finish(&msbc);

} END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_msbc_init);
	tcase_add_test(tc, test_msbc_find_h2_header);
	tcase_add_test(tc, test_msbc_encode_decode);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
