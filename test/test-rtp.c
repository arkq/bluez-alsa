/*
 * test-rtp.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <check.h>

#include "rtp.h"
#include "shared/defs.h"

#include "inc/check.inc"

CK_START_TEST(test_rtp_a2dp_init) {

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

} CK_END_TEST

CK_START_TEST(test_rtp_a2dp_get_payload) {

	uint8_t buffer[sizeof(rtp_header_t) + 16];
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

} CK_END_TEST

CK_START_TEST(test_rtp_state_new_frame) {

	struct rtp_state rtp;
	rtp_state_init(&rtp, 8000, 8000);

	rtp_header_t header = { 0 };
	int sn_offset = rtp.seq_number;
	int ts_offset = rtp.ts_offset;

	for (size_t i = 1; i <= 16; i++) {
		rtp_state_new_frame(&rtp, &header);
		ck_assert_int_eq(be16toh(header.seq_number), sn_offset + i);
		ck_assert_int_eq(be32toh(header.timestamp), ts_offset);
	}

} CK_END_TEST

CK_START_TEST(test_rtp_state_sync_stream) {

	struct rtp_state rtp;
	rtp_state_init(&rtp, 8000, 8000);

	struct {
		bool ok;
		struct {
			rtp_header_t header;
			size_t pcm_frames;
		} rtp;
		struct {
			int missing_rtp;
			int missing_pcm;
		} assert;
	} stream[] = {

#define RTP(SEQ, TS, PCM_FRAMES) \
	{ { .seq_number = htobe16(SEQ), .timestamp = htobe32(TS) }, PCM_FRAMES }

		{ 1, RTP(1, 10, 10), { 0, 0 } },

		/* missing single RTP packet */
		{ 0, RTP(2, 20, 10), { 0, 0 } },

		{ 1, RTP(3, 30, 0), { 1, 10 } },
		{ 1, RTP(4, 30, 10), { 0, 0 } },

		/* fragmented RTP - missing first fragment */
		{ 0, RTP(5, 40, 0), { 0, 0 } },
		{ 1, RTP(6, 40, 10), { 1, 0 } },

		{ 1, RTP(7, 50, 10), { 0, 10 } },

		/* fragmented RTP - missing middle fragment */
		{ 1, RTP(8, 60, 0), { 0, 0 } },
		{ 0, RTP(9, 60, 0), { 0, 0 } },
		{ 1, RTP(10, 60, 10), { 1, 0 } },

		/* missing single RTP packet just after broken fragmentation */
		{ 0, RTP(11, 70, 10), { 0, 0 } },

		/* yet another missing single RTP packet */
		{ 0, RTP(12, 80, 10), { 0, 0 } },

		{ 1, RTP(13, 90, 20), { 2, 30 } },

		/* fragmented RTP - missing more than one fragment */
		{ 1, RTP(14, 110, 0), { 0, 0 } },
		{ 0, RTP(15, 110, 0), { 0, 0 } },
		{ 1, RTP(16, 110, 0), { 1, 0 } },
		{ 0, RTP(17, 110, 10), { 0, 0 } },

		{ 1, RTP(18, 120, 10), { 1, 10 } },

	};

	uint32_t ts_missing = 0;
	for (size_t i = 0; i < ARRAYSIZE(stream); i++) {

		if (!stream[i].ok) {
			ts_missing = stream[i].rtp.header.timestamp;
			continue;
		}

		int missing_pcm_frames = 0;
		int missing_rtp_frames = 0;
		rtp_state_sync_stream(&rtp, &stream[i].rtp.header,
				&missing_rtp_frames, &missing_pcm_frames);

		ck_assert_int_eq(missing_pcm_frames, stream[i].assert.missing_pcm);
		ck_assert_int_eq(missing_rtp_frames, stream[i].assert.missing_rtp);

		/* if (fragmented) packet was not broken, simulate PCM playback */
		if (ts_missing != stream[i].rtp.header.timestamp &&
				stream[i].rtp.pcm_frames)
			rtp_state_update(&rtp, stream[i].rtp.pcm_frames);

	}

} CK_END_TEST

CK_START_TEST(test_rtp_state_update) {

	struct rtp_state rtp;
	rtp_state_init(&rtp, 8000, 8000);

	for (size_t i = 0; i < 16; i++)
		rtp_state_update(&rtp, 10);

	ck_assert_int_eq(rtp.ts_pcm_frames, 10 * 16);

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_rtp_a2dp_init);
	tcase_add_test(tc, test_rtp_a2dp_get_payload);

	tcase_add_test(tc, test_rtp_state_new_frame);
	tcase_add_test(tc, test_rtp_state_sync_stream);
	tcase_add_test(tc, test_rtp_state_update);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
