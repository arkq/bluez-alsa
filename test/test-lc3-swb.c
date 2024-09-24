/*
 * test-lc3-swb.c
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

#include "codec-lc3-swb.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"

#include "inc/check.inc"
#include "inc/sine.inc"

CK_START_TEST(test_lc3_swb_init) {

	struct esco_lc3_swb lc3_swb;

	lc3_swb_init(&lc3_swb);
	ck_assert_int_eq(ffb_len_out(&lc3_swb.pcm), 0);

	ffb_seek(&lc3_swb.pcm, 16);
	ck_assert_int_eq(ffb_len_out(&lc3_swb.pcm), 16);

	lc3_swb_init(&lc3_swb);
	ck_assert_int_eq(ffb_len_out(&lc3_swb.pcm), 0);

} CK_END_TEST

CK_START_TEST(test_lc3_swb_encode_decode) {

	int16_t sine[8 * LC3_SWB_CODESAMPLES];
	snd_pcm_sine_s16_2le(sine, 1, ARRAYSIZE(sine), 1.0 / 128, 0);

	uint8_t data[sizeof(sine)];
	uint8_t *data_tail = data;

	struct esco_lc3_swb lc3_swb;
	size_t len;
	size_t i;
	int rv;

	lc3_swb_init(&lc3_swb);
	for (rv = 1, i = 0; rv > 0;) {

		len = MIN(ARRAYSIZE(sine) - i, ffb_len_in(&lc3_swb.pcm));
		memcpy(lc3_swb.pcm.tail, &sine[i], len * lc3_swb.pcm.size);
		ffb_seek(&lc3_swb.pcm, len);
		i += len;

		rv = lc3_swb_encode(&lc3_swb);

		len = ffb_blen_out(&lc3_swb.data);
		memcpy(data_tail, lc3_swb.data.data, len);
		ffb_rewind(&lc3_swb.data);
		data_tail += len;

	}

	ck_assert_int_eq(data_tail - data, 480);

	int16_t pcm[sizeof(sine)];
	int16_t *pcm_tail = pcm;

	lc3_swb_init(&lc3_swb);
	for (rv = 1, i = 0; rv > 0; ) {

		len = MIN((data_tail - data) - i, ffb_blen_in(&lc3_swb.data));
		memcpy(lc3_swb.data.tail, &data[i], len);
		ffb_seek(&lc3_swb.data, len);
		i += len;

		rv = lc3_swb_decode(&lc3_swb);

		len = ffb_len_out(&lc3_swb.pcm);
		memcpy(pcm_tail, lc3_swb.pcm.data, len * lc3_swb.pcm.size);
		ffb_rewind(&lc3_swb.pcm);
		pcm_tail += len;

	}

	ck_assert_int_eq(pcm_tail - pcm, 8 * LC3_SWB_CODESAMPLES);

} CK_END_TEST

CK_START_TEST(test_lc3_swb_decode_plc) {

	int16_t sine[18 * LC3_SWB_CODESAMPLES];
	snd_pcm_sine_s16_2le(sine, 1, ARRAYSIZE(sine), 1.0 / 128, 0);

	struct esco_lc3_swb lc3_swb;
	lc3_swb_init(&lc3_swb);

	uint8_t data[sizeof(sine)];
	uint8_t *data_tail = data;

	debug("Simulating eSCO packet loss events");

	int rv;
	size_t counter, i;
	for (rv = 1, counter = i = 0; rv > 0; counter++) {

		bool packet_error = false;
		size_t len = MIN(ARRAYSIZE(sine) - i, ffb_len_in(&lc3_swb.pcm));
		memcpy(lc3_swb.pcm.tail, &sine[i], len * lc3_swb.pcm.size);
		ffb_seek(&lc3_swb.pcm, len);
		i += len;

		rv = lc3_swb_encode(&lc3_swb);

		len = ffb_blen_out(&lc3_swb.data);
		memcpy(data_tail, lc3_swb.data.data, len);
		ffb_rewind(&lc3_swb.data);

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
			data_tail[16] *= 0x07;
			packet_error = true;
		}

		fprintf(stderr, packet_error ? "e" : "x");
		data_tail += len;

	}

	fprintf(stderr, "\n");

	/* reinitialize encoder/decoder handler */
	lc3_swb_init(&lc3_swb);

	size_t samples = 0;
	for (rv = 1, i = 0; rv > 0; ) {

		size_t len = MIN((data_tail - data) - i, ffb_blen_in(&lc3_swb.data));
		memcpy(lc3_swb.data.tail, &data[i], len);
		ffb_seek(&lc3_swb.data, len);
		i += len;

		rv = lc3_swb_decode(&lc3_swb);

		samples += ffb_len_out(&lc3_swb.pcm);
		ffb_rewind(&lc3_swb.pcm);

	}

	/* we should recover all except consecutive 4 frames */
	ck_assert_int_eq(samples, (18 - 4) * LC3_SWB_CODESAMPLES);

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_lc3_swb_init);
	tcase_add_test(tc, test_lc3_swb_encode_decode);
	tcase_add_test(tc, test_lc3_swb_decode_plc);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
