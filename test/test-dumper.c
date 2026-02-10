/*
 * test-bt-advertising.c
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <check.h>

#include "ba-transport.h"
#include "dumper/dumper.h"
#include "shared/bluetooth-a2dp.h"
#include "shared/bluetooth-hfp.h"
#include "shared/log.h"

#include "inc/check.inc"

CK_START_TEST(test_ba_dumper_profile_to_mask) {
	ck_assert_uint_eq(ba_dumper_profile_to_mask(BA_TRANSPORT_PROFILE_HSP_AG),
			BA_TRANSPORT_PROFILE_MASK_HSP);
} CK_END_TEST

CK_START_TEST(test_ba_dumper_profile_mask_from_string) {
	ck_assert_uint_eq(ba_dumper_profile_mask_from_string("A2DP"), BA_TRANSPORT_PROFILE_MASK_A2DP);
} CK_END_TEST

CK_START_TEST(test_ba_dumper_profile_mask_to_string) {
	ck_assert_str_eq(ba_dumper_profile_mask_to_string(BA_TRANSPORT_PROFILE_MASK_A2DP), "A2DP");
} CK_END_TEST

CK_START_TEST(test_ba_dumper_read_header_simple) {

	char data[] = "HFP:CVSD\n";
	FILE * f = fmemopen(data, sizeof(data), "r");

	uint32_t profile;
	uint32_t codec;
	uint8_t conf[256];
	size_t size = sizeof(conf);
	ck_assert_int_eq(ba_dumper_read_header(f, &profile, &codec, conf, &size), strlen(data));
	ck_assert_uint_eq(profile, BA_TRANSPORT_PROFILE_MASK_HFP);
	ck_assert_uint_eq(codec, HFP_CODEC_CVSD);
	ck_assert_uint_eq(size, 0);

	fclose(f);

} CK_END_TEST

CK_START_TEST(test_ba_dumper_write_header_simple) {

	const struct ba_transport t = {
		.profile = BA_TRANSPORT_PROFILE_HSP_AG,
		.codec_id_mtx = PTHREAD_MUTEX_INITIALIZER,
		.codec_id = HFP_CODEC_CVSD,
	};

	char buffer[256] = { 0 };
	FILE * f = fmemopen(buffer, sizeof(buffer), "w");

	const char expected[] = "HSP:CVSD\n";
	ck_assert_int_eq(ba_dumper_write_header(f, &t), sizeof(expected) - 1);
	/* Flush the stream by closing it. */
	fclose(f);

	ck_assert_str_eq(buffer, expected);

} CK_END_TEST

CK_START_TEST(test_ba_dumper_read_header_full) {

	char data[] = "A2DP:SBC:ffff0235\n";
	const uint8_t configuration[] = { 0xff, 0xff, 0x02, 0x35 };
	FILE * f = fmemopen(data, sizeof(data), "r");

	uint32_t profile;
	uint32_t codec;
	uint8_t conf[256];
	size_t size = sizeof(conf);
	ck_assert_int_eq(ba_dumper_read_header(f, &profile, &codec, conf, &size), strlen(data));
	ck_assert_uint_eq(profile, BA_TRANSPORT_PROFILE_MASK_A2DP);
	ck_assert_uint_eq(codec, A2DP_CODEC_SBC);
	ck_assert_mem_eq(conf, configuration, sizeof(configuration));
	ck_assert_uint_eq(size, sizeof(configuration));

	fclose(f);

} CK_END_TEST

CK_START_TEST(test_ba_dumper_write_header_full) {

	const struct a2dp_sep sep = {
		.config.type = A2DP_SOURCE,
		.config.codec_id = A2DP_CODEC_SBC,
		.config.caps_size = sizeof(a2dp_sbc_t) };
	const struct ba_transport t = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec_id_mtx = PTHREAD_MUTEX_INITIALIZER,
		.codec_id = A2DP_CODEC_SBC,
		.media.a2dp.configuration.sbc.channel_mode = SBC_CHANNEL_MODE_STEREO,
		.media.a2dp.sep = &sep,
	};

	char buffer[256] = { 0 };
	FILE * f = fmemopen(buffer, sizeof(buffer), "w");

	const char expected[] = "A2DP:SBC:02000000\n";
	ck_assert_int_eq(ba_dumper_write_header(f, &t), sizeof(expected) - 1);
	/* Flush the stream by closing it. */
	fclose(f);

	ck_assert_str_eq(buffer, expected);

} CK_END_TEST

CK_START_TEST(test_ba_dumper_read) {

	char data[] = "000A 0123456789abcdef0123\n";
	const uint8_t packet[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23 };
	FILE * f = fmemopen(data, sizeof(data), "r");

	uint8_t buffer[256];
	ck_assert_int_eq(ba_dumper_read(f, buffer, sizeof(buffer)), sizeof(packet));
	ck_assert_mem_eq(buffer, packet, sizeof(packet));

	fclose(f);

} CK_END_TEST

CK_START_TEST(test_ba_dumper_write) {

	char buffer[256] = { 0 };
	FILE * f = fmemopen(buffer, sizeof(buffer), "w");

	const char expected[] = "0008 0123456789abcdef\n";
	const uint8_t packet[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
	ck_assert_int_eq(ba_dumper_write(f, packet, sizeof(packet)), sizeof(expected) - 1);
	/* Flush the stream by closing it. */
	fclose(f);

	ck_assert_str_eq(buffer, expected);

} CK_END_TEST

CK_START_TEST(test_ba_transport_to_string) {
	const struct ba_transport t = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec_id_mtx = PTHREAD_MUTEX_INITIALIZER,
		.codec_id = A2DP_CODEC_SBC };
	ck_assert_str_eq(ba_transport_to_string(&t), "A2DP-SBC");
} CK_END_TEST

CK_START_TEST(test_ba_transport_pcm_to_string) {
	const struct ba_transport_pcm t_pcm = {
		.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE,
		.channels = 2,
		.rate = 44100 };
	ck_assert_str_eq(ba_transport_pcm_to_string(&t_pcm), "s16-44100-2c");
} CK_END_TEST

int main(void) {

	Suite * s = suite_create(__FILE__);
	TCase * tc = tcase_create(__FILE__);
	SRunner * sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_ba_dumper_profile_to_mask);
	tcase_add_test(tc, test_ba_dumper_profile_mask_from_string);
	tcase_add_test(tc, test_ba_dumper_profile_mask_to_string);
	tcase_add_test(tc, test_ba_dumper_read_header_simple);
	tcase_add_test(tc, test_ba_dumper_write_header_simple);
	tcase_add_test(tc, test_ba_dumper_read_header_full);
	tcase_add_test(tc, test_ba_dumper_write_header_full);
	tcase_add_test(tc, test_ba_dumper_read);
	tcase_add_test(tc, test_ba_dumper_write);

	tcase_add_test(tc, test_ba_transport_to_string);
	tcase_add_test(tc, test_ba_transport_pcm_to_string);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
