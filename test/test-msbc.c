/*
 * test-msbc.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <check.h>
#include <glib.h>

#include "codec-msbc.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"

#include "inc/sine.inc"
#include "../src/codec-msbc.c"

START_TEST(test_msbc_init) {

	struct esco_msbc msbc = { .initialized = false };

	ck_assert_int_eq(msbc_init(&msbc), 0);
	ck_assert_int_eq(msbc.initialized, true);
	ck_assert_int_eq(ffb_len_out(&msbc.pcm), 0);

	ffb_seek(&msbc.pcm, 16);
	ck_assert_int_eq(ffb_len_out(&msbc.pcm), 16);

	ck_assert_int_eq(msbc_init(&msbc), 0);
	ck_assert_int_eq(msbc.initialized, true);
	ck_assert_int_eq(ffb_len_out(&msbc.pcm), 0);

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

	int16_t sine[8 * MSBC_CODESAMPLES];
	snd_pcm_sine_s16_2le(sine, ARRAYSIZE(sine), 1, 0, 1.0 / 128);

	uint8_t data[sizeof(sine)];
	uint8_t *data_tail = data;

	struct esco_msbc msbc = { 0 };
	size_t len;
	size_t i;
	int rv;

	msbc.initialized = false;
	ck_assert_int_eq(msbc_init(&msbc), 0);
	for (rv = 1, i = 0; rv > 0;) {

		len = MIN(ARRAYSIZE(sine) - i, ffb_len_in(&msbc.pcm));
		memcpy(msbc.pcm.tail, &sine[i], len * msbc.pcm.size);
		ffb_seek(&msbc.pcm, len);
		i += len;

		rv = msbc_encode(&msbc);

		len = ffb_blen_out(&msbc.data);
		memcpy(data_tail, msbc.data.data, len);
		ffb_rewind(&msbc.data);
		data_tail += len;

	}

	ck_assert_int_eq(data_tail - data, 480);

	msbc_finish(&msbc);

	int16_t pcm[sizeof(sine)];
	int16_t *pcm_tail = pcm;

	msbc.initialized = false;
	ck_assert_int_eq(msbc_init(&msbc), 0);
	for (rv = 1, i = 0; rv > 0; ) {

		len = MIN((data_tail - data) - i, ffb_blen_in(&msbc.data));
		memcpy(msbc.data.tail, &data[i], len);
		ffb_seek(&msbc.data, len);
		i += len;

		rv = msbc_decode(&msbc);

		len = ffb_len_out(&msbc.pcm);
		memcpy(pcm_tail, msbc.pcm.data, len * msbc.pcm.size);
		ffb_rewind(&msbc.pcm);
		pcm_tail += len;

	}

	ck_assert_int_eq(pcm_tail - pcm, 960);

	msbc_finish(&msbc);

} END_TEST

START_TEST(test_msbc_decode_plc) {

	int16_t sine[18 * MSBC_CODESAMPLES];
	snd_pcm_sine_s16_2le(sine, ARRAYSIZE(sine), 1, 0, 1.0 / 128);

	struct esco_msbc msbc = { .initialized = false };
	ck_assert_int_eq(msbc_init(&msbc), 0);

	uint8_t data[sizeof(sine)];
	uint8_t *data_tail = data;

	debug("Simulating mSBC packet loss events");

	int rv;
	size_t counter, i;
	for (rv = 1, counter = i = 0; rv > 0; counter++) {

		size_t len = MIN(ARRAYSIZE(sine) - i, ffb_len_in(&msbc.pcm));
		memcpy(msbc.pcm.tail, &sine[i], len * msbc.pcm.size);
		ffb_seek(&msbc.pcm, len);
		i += len;

		rv = msbc_encode(&msbc);

		len = ffb_blen_out(&msbc.data);
		memcpy(data_tail, msbc.data.data, len);
		ffb_rewind(&msbc.data);

		/* simulate packet loss */
		if (counter == 2 ||
				(6 <= counter && counter <= 8) ||
				/* 4 packets (undetectable) */
				(12 <= counter && counter <= 15)) {
			fprintf(stderr, "_");
			continue;
		}

		fprintf(stderr, "x");
		data_tail += len;

	}

	fprintf(stderr, "\n");

	/* reinitialize encoder/decoder handler */
	ck_assert_int_eq(msbc_init(&msbc), 0);

	size_t samples = 0;
	for (rv = 1, i = 0; rv > 0; ) {

		size_t len = MIN((data_tail - data) - i, ffb_blen_in(&msbc.data));
		memcpy(msbc.data.tail, &data[i], len);
		ffb_seek(&msbc.data, len);
		i += len;

		rv = msbc_decode(&msbc);

		samples += ffb_len_out(&msbc.pcm);
		ffb_rewind(&msbc.pcm);

	}

	/* we should recover all except consecutive 4 frames */
	ck_assert_int_eq(samples, (18 - 4) * MSBC_CODESAMPLES);

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
	tcase_add_test(tc, test_msbc_decode_plc);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
