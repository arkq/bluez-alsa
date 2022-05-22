/*
 * test-rtp.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdint.h>
#include <string.h>

#include <check.h>

#include "rtp.h"

START_TEST(test_rtp_a2dp_init) {

	uint8_t buffer[RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 16];
	for (size_t i = 0; i < sizeof(buffer); i++)
		buffer[i] = i;

	rtp_header_t *header;
	rtp_media_header_t *media;
	uint8_t *payload;

	payload = rtp_a2dp_init(buffer, &header, (void **)&media, sizeof(*media));
	ck_assert_int_eq(header->paytype, 96);
	ck_assert_int_eq(header->version, 2);
	ck_assert_ptr_ne(payload, NULL);
	ck_assert_int_eq(payload[0], 13);

} END_TEST

START_TEST(test_rtp_a2dp_get_payload) {

	uint8_t buffer[RTP_HEADER_LEN + 16];
	for (size_t i = 0; i < sizeof(buffer); i++)
		buffer[i] = i;

	rtp_header_t *header = (rtp_header_t *)buffer;
	uint8_t *payload;

#if ENABLE_PAYLOADCHECK
	payload = rtp_a2dp_get_payload(header);
	ck_assert_ptr_eq(payload, NULL);
#endif

	header->paytype = 96;
	payload = rtp_a2dp_get_payload(header);
	ck_assert_ptr_ne(payload, NULL);
	ck_assert_int_eq(payload[0], 12);

} END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_rtp_a2dp_init);
	tcase_add_test(tc, test_rtp_a2dp_get_payload);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
