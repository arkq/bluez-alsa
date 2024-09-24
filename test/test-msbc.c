/*
 * test-msbc.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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

#include "inc/check.inc"
#include "inc/sine.inc"

CK_START_TEST(test_msbc_init) {

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

} CK_END_TEST

CK_START_TEST(test_msbc_encode_decode) {

	int16_t sine[8 * MSBC_CODESAMPLES];
	snd_pcm_sine_s16_2le(sine, 1, ARRAYSIZE(sine), 1.0 / 128, 0);

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

	ck_assert_int_eq(pcm_tail - pcm, 8 * MSBC_CODESAMPLES);

	msbc_finish(&msbc);

} CK_END_TEST

CK_START_TEST(test_msbc_decode_plc) {

	int16_t sine[18 * MSBC_CODESAMPLES];
	snd_pcm_sine_s16_2le(sine, 1, ARRAYSIZE(sine), 1.0 / 128, 0);

	struct esco_msbc msbc = { .initialized = false };
	ck_assert_int_eq(msbc_init(&msbc), 0);

	uint8_t data[sizeof(sine)];
	uint8_t *data_tail = data;

	debug("Simulating eSCO packet loss events");

	int rv;
	size_t counter, i;
	for (rv = 1, counter = i = 0; rv > 0; counter++) {

		bool packet_error = false;
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

		/* simulate packet error */
		if (counter == 4) {
			data_tail[5] *= 0x07;
			packet_error = true;
		}

		fprintf(stderr, packet_error ? "e" : "x");
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

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_msbc_init);
	tcase_add_test(tc, test_msbc_encode_decode);
	tcase_add_test(tc, test_msbc_decode_plc);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
