/*
 * test-a2dp.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <check.h>
#include <glib.h>

#include "a2dp.h"
#include "a2dp-aac.h"
#include "a2dp-aptx.h"
#include "a2dp-sbc.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-config.h"
#include "codec-sbc.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

#include "inc/check.inc"

const char *ba_transport_debug_name(const struct ba_transport *t) { (void)t; return "x"; }
uint16_t ba_transport_get_codec(const struct ba_transport *t) { (void)t; return 0; }
bool ba_transport_pcm_is_active(const struct ba_transport_pcm *pcm) { (void)pcm; return false; }
int ba_transport_pcm_release(struct ba_transport_pcm *pcm) { (void)pcm; return -1; }
int ba_transport_stop_if_no_clients(struct ba_transport *t) { (void)t; return -1; }
int ba_transport_pcm_bt_release(struct ba_transport_pcm *pcm) { (void)pcm; return -1; }
int ba_transport_pcm_start(struct ba_transport_pcm *pcm,
		ba_transport_pcm_thread_func th_func, const char *name) {
	(void)pcm; (void)th_func; (void)name; return -1; }
int ba_transport_pcm_state_set(struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_state state) {
	(void)pcm; (void)state; return -1; }
enum ba_transport_pcm_signal ba_transport_pcm_signal_recv(struct ba_transport_pcm *pcm) {
	(void)pcm; return -1; }
void ba_transport_pcm_thread_cleanup(struct ba_transport_pcm *pcm) { (void)pcm; }

CK_START_TEST(test_a2dp_codecs_codec_id_from_string) {
	ck_assert_int_eq(a2dp_codecs_codec_id_from_string("SBC"), A2DP_CODEC_SBC);
	ck_assert_int_eq(a2dp_codecs_codec_id_from_string("apt-x"), A2DP_CODEC_VENDOR_APTX);
	ck_assert_int_eq(a2dp_codecs_codec_id_from_string("unknown"), 0xFFFF);
} CK_END_TEST

CK_START_TEST(test_a2dp_codecs_codec_id_to_string) {
	ck_assert_str_eq(a2dp_codecs_codec_id_to_string(A2DP_CODEC_SBC), "SBC");
	ck_assert_str_eq(a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_APTX), "aptX");
	ck_assert_ptr_eq(a2dp_codecs_codec_id_to_string(0xFFFF), NULL);
} CK_END_TEST

CK_START_TEST(test_a2dp_codecs_get_canonical_name) {
	ck_assert_str_eq(a2dp_codecs_get_canonical_name("apt-x"), "aptX");
	ck_assert_str_eq(a2dp_codecs_get_canonical_name("Foo-Bar"), "Foo-Bar");
} CK_END_TEST

CK_START_TEST(test_a2dp_dir) {
	ck_assert_int_eq(A2DP_SOURCE, !A2DP_SINK);
	ck_assert_int_eq(!A2DP_SOURCE, A2DP_SINK);
} CK_END_TEST

CK_START_TEST(test_a2dp_codecs_init) {
	a2dp_codecs_init();
} CK_END_TEST

CK_START_TEST(test_a2dp_codec_cmp) {

	struct a2dp_codec c1 = { .dir = A2DP_SOURCE, .codec_id = A2DP_CODEC_SBC };
	struct a2dp_codec c2 = { .dir = A2DP_SOURCE, .codec_id = A2DP_CODEC_MPEG24 };
	struct a2dp_codec c3 = { .dir = A2DP_SOURCE, .codec_id = A2DP_CODEC_VENDOR_APTX };
	struct a2dp_codec c4 = { .dir = A2DP_SINK, .codec_id = A2DP_CODEC_SBC };
	struct a2dp_codec c5 = { .dir = A2DP_SINK, .codec_id = A2DP_CODEC_VENDOR_APTX };
	struct a2dp_codec c6 = { .dir = A2DP_SINK, .codec_id = A2DP_CODEC_VENDOR_LDAC };
	struct a2dp_codec c7 = { .dir = A2DP_SINK, .codec_id = 0xFFFF };

	struct a2dp_codec * codecs[] = { &c3, &c1, &c6, &c4, &c7, &c5, &c2 };
	qsort(codecs, ARRAYSIZE(codecs), sizeof(*codecs), QSORT_COMPAR(a2dp_codec_ptr_cmp));

	ck_assert_ptr_eq(codecs[0], &c1);
	ck_assert_ptr_eq(codecs[1], &c2);
	ck_assert_ptr_eq(codecs[2], &c3);
	ck_assert_ptr_eq(codecs[3], &c4);
	ck_assert_ptr_eq(codecs[4], &c5);
	ck_assert_ptr_eq(codecs[5], &c6);
	ck_assert_ptr_eq(codecs[6], &c7);

} CK_END_TEST

CK_START_TEST(test_a2dp_sep_cmp) {

	struct a2dp_sep seps[] = {
		{ .dir = A2DP_SOURCE, .codec_id = A2DP_CODEC_VENDOR_APTX },
		{ .dir = A2DP_SINK, .codec_id = A2DP_CODEC_SBC },
		{ .dir = A2DP_SINK, .codec_id = A2DP_CODEC_VENDOR_APTX },
		{ .dir = A2DP_SOURCE, .codec_id = A2DP_CODEC_MPEG24 },
		{ .dir = A2DP_SOURCE, .codec_id = A2DP_CODEC_SBC } };
	qsort(seps, ARRAYSIZE(seps), sizeof(*seps), QSORT_COMPAR(a2dp_sep_cmp));

	ck_assert_int_eq(seps[0].codec_id, A2DP_CODEC_SBC);
	ck_assert_int_eq(seps[1].codec_id, A2DP_CODEC_MPEG24);
	ck_assert_int_eq(seps[2].codec_id, A2DP_CODEC_VENDOR_APTX);
	ck_assert_int_eq(seps[3].dir, A2DP_SINK);
	ck_assert_int_eq(seps[3].codec_id, A2DP_CODEC_SBC);
	ck_assert_int_eq(seps[4].dir, A2DP_SINK);
	ck_assert_int_eq(seps[4].codec_id, A2DP_CODEC_VENDOR_APTX);

} CK_END_TEST

CK_START_TEST(test_a2dp_codec_lookup) {
	ck_assert_ptr_eq(a2dp_codec_lookup(A2DP_CODEC_SBC, A2DP_SOURCE), &a2dp_sbc_source);
	ck_assert_ptr_eq(a2dp_codec_lookup(0xFFFF, A2DP_SOURCE), NULL);
} CK_END_TEST

CK_START_TEST(test_a2dp_get_vendor_codec_id) {

	uint8_t cfg0[4] = { 0xDE, 0xAD, 0xB0, 0xBE };
	ck_assert_int_eq(a2dp_get_vendor_codec_id(cfg0, sizeof(cfg0)), 0xFFFF);
	ck_assert_int_eq(errno, EINVAL);

	a2dp_aptx_t cfg1 = { A2DP_VENDOR_INFO_INIT(APTX_VENDOR_ID, APTX_CODEC_ID), 0, 0 };
	ck_assert_int_eq(a2dp_get_vendor_codec_id(&cfg1, sizeof(cfg1)), A2DP_CODEC_VENDOR_APTX);

	a2dp_aptx_t cfg2 = { A2DP_VENDOR_INFO_INIT(APTX_VENDOR_ID, 0x69), 0, 0 };
	ck_assert_int_eq(a2dp_get_vendor_codec_id(&cfg2, sizeof(cfg2)), 0xFFFF);
	ck_assert_int_eq(errno, ENOTSUP);

} CK_END_TEST

CK_START_TEST(test_a2dp_check_configuration) {

	const a2dp_sbc_t cfg_valid = {
		.frequency = SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_STEREO,
		.block_length = SBC_BLOCK_LENGTH_8,
		.subbands = SBC_SUBBANDS_8,
		.allocation_method = SBC_ALLOCATION_SNR,
		.min_bitpool = 42,
		.max_bitpool = 62,
	};

	ck_assert_int_eq(a2dp_check_configuration(&a2dp_sbc_source,
			&cfg_valid, sizeof(cfg_valid) + 1), A2DP_CHECK_ERR_SIZE);

	ck_assert_int_eq(a2dp_check_configuration(&a2dp_sbc_source,
				&cfg_valid, sizeof(cfg_valid)), A2DP_CHECK_OK);

	const a2dp_sbc_t cfg_invalid = {
		.frequency = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_STEREO | SBC_CHANNEL_MODE_JOINT_STEREO,
		.block_length = SBC_BLOCK_LENGTH_8,
		.allocation_method = SBC_ALLOCATION_SNR,
	};

	ck_assert_int_eq(a2dp_check_configuration(&a2dp_sbc_source,
				&cfg_invalid, sizeof(cfg_invalid)), A2DP_CHECK_ERR_SAMPLING);

#if ENABLE_AAC
	a2dp_aac_t cfg_aac_invalid = {
		/* FDK-AAC encoder does not support AAC Long Term Prediction */
		.object_type = AAC_OBJECT_TYPE_MPEG4_LTP,
		A2DP_AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_44100)
		.channels = AAC_CHANNELS_1 };
	ck_assert_int_eq(a2dp_check_configuration(&a2dp_aac_source,
			&cfg_aac_invalid, sizeof(cfg_aac_invalid)), A2DP_CHECK_ERR_OBJECT_TYPE);
#endif

} CK_END_TEST

CK_START_TEST(test_a2dp_check_strerror) {
	ck_assert_str_eq(a2dp_check_strerror(A2DP_CHECK_ERR_SIZE), "Invalid size");
	ck_assert_str_eq(a2dp_check_strerror(0xFFFF), "Check error");
} CK_END_TEST

CK_START_TEST(test_a2dp_filter_capabilities) {

	a2dp_sbc_t caps_sbc = {
		.frequency = SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_STEREO,
		.block_length = SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8,
		.subbands = SBC_SUBBANDS_4,
		.allocation_method = SBC_ALLOCATION_SNR,
		.min_bitpool = 42,
		.max_bitpool = 255,
	};

#if ENABLE_APTX
	a2dp_aptx_t caps_aptx = {
		.info = A2DP_VENDOR_INFO_INIT(APTX_VENDOR_ID, APTX_CODEC_ID),
		.frequency = APTX_SAMPLING_FREQ_32000 | APTX_SAMPLING_FREQ_44100,
		.channel_mode = APTX_CHANNEL_MODE_MONO | APTX_CHANNEL_MODE_STEREO,
	};
#endif

	ck_assert_int_eq(a2dp_filter_capabilities(&a2dp_sbc_source,
			&a2dp_sbc_source.capabilities, &caps_sbc, sizeof(caps_sbc) + 1), -1);
	ck_assert_int_eq(errno, EINVAL);

	hexdump("Capabilities A", &caps_sbc, sizeof(caps_sbc), true);
	hexdump("Capabilities B", &a2dp_sbc_source.capabilities, sizeof(caps_sbc), true);
	ck_assert_int_eq(a2dp_filter_capabilities(&a2dp_sbc_source,
			&a2dp_sbc_source.capabilities, &caps_sbc, sizeof(caps_sbc)), 0);

	hexdump("Capabilities filtered", &caps_sbc, sizeof(caps_sbc), true);
	ck_assert_int_eq(caps_sbc.frequency, SBC_SAMPLING_FREQ_44100);
	ck_assert_int_eq(caps_sbc.channel_mode, SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_STEREO);
	ck_assert_int_eq(caps_sbc.block_length, SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8);
	ck_assert_int_eq(caps_sbc.subbands, SBC_SUBBANDS_4);
	ck_assert_int_eq(caps_sbc.allocation_method, SBC_ALLOCATION_SNR);
	ck_assert_int_eq(caps_sbc.min_bitpool, MAX(SBC_MIN_BITPOOL, 42));
	ck_assert_int_eq(caps_sbc.max_bitpool, MIN(SBC_MAX_BITPOOL, 255));

#if ENABLE_APTX
	/* Check whether generic bitwise AND filtering works correctly. */
	hexdump("Capabilities A", &caps_aptx, sizeof(caps_aptx), true);
	hexdump("Capabilities B", &a2dp_aptx_source.capabilities, sizeof(caps_aptx), true);
	ck_assert_int_eq(a2dp_filter_capabilities(&a2dp_aptx_source,
			&a2dp_aptx_source.capabilities, &caps_aptx, sizeof(caps_aptx)), 0);
	hexdump("Capabilities filtered", &caps_aptx, sizeof(caps_aptx), true);
	ck_assert_int_eq(caps_aptx.frequency, APTX_SAMPLING_FREQ_32000 | APTX_SAMPLING_FREQ_44100);
	ck_assert_int_eq(caps_aptx.channel_mode, APTX_CHANNEL_MODE_STEREO);
#endif

} CK_END_TEST

CK_START_TEST(test_a2dp_select_configuration) {

	a2dp_sbc_t cfg;
	const a2dp_sbc_t cfg_ = {
		.frequency = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_44100 | SBC_SAMPLING_FREQ_48000,
		.channel_mode = SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_DUAL_CHANNEL | SBC_CHANNEL_MODE_STEREO,
		.block_length = SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8,
		.subbands = SBC_SUBBANDS_4 | SBC_SUBBANDS_8,
		.allocation_method = SBC_ALLOCATION_SNR | SBC_ALLOCATION_LOUDNESS,
		.min_bitpool = 42,
		.max_bitpool = 255,
	};

	cfg = cfg_;
	ck_assert_int_eq(a2dp_select_configuration(&a2dp_sbc_source, &cfg, sizeof(cfg) + 1), -1);
	ck_assert_int_eq(errno, EINVAL);

	cfg = cfg_;
	ck_assert_int_eq(a2dp_select_configuration(&a2dp_sbc_source, &cfg, sizeof(cfg)), 0);
	ck_assert_int_eq(cfg.frequency, SBC_SAMPLING_FREQ_48000);
	ck_assert_int_eq(cfg.channel_mode, SBC_CHANNEL_MODE_STEREO);
	ck_assert_int_eq(cfg.block_length, SBC_BLOCK_LENGTH_8);
	ck_assert_int_eq(cfg.subbands, SBC_SUBBANDS_8);
	ck_assert_int_eq(cfg.allocation_method, SBC_ALLOCATION_LOUDNESS);
	ck_assert_int_eq(cfg.min_bitpool, 42);
	ck_assert_int_eq(cfg.max_bitpool, 250);

	cfg = cfg_;
	config.a2dp.force_mono = true;
	ck_assert_int_eq(a2dp_select_configuration(&a2dp_sbc_source, &cfg, sizeof(cfg)), 0);
	ck_assert_int_eq(cfg.channel_mode, SBC_CHANNEL_MODE_MONO);

	cfg = cfg_;
	config.a2dp.force_mono = false;
	config.a2dp.force_44100 = true;
	config.sbc_quality = SBC_QUALITY_XQ;
	ck_assert_int_eq(a2dp_select_configuration(&a2dp_sbc_source, &cfg, sizeof(cfg)), 0);
	ck_assert_int_eq(cfg.frequency, SBC_SAMPLING_FREQ_44100);
	ck_assert_int_eq(cfg.channel_mode, SBC_CHANNEL_MODE_DUAL_CHANNEL);
	ck_assert_int_eq(cfg.block_length, SBC_BLOCK_LENGTH_8);
	ck_assert_int_eq(cfg.subbands, SBC_SUBBANDS_8);
	ck_assert_int_eq(cfg.allocation_method, SBC_ALLOCATION_LOUDNESS);
	ck_assert_int_eq(cfg.min_bitpool, 42);
	ck_assert_int_eq(cfg.max_bitpool, 250);

#if ENABLE_AAC

	a2dp_aac_t cfg_aac;
	const a2dp_aac_t cfg_aac_ = {
		.object_type = AAC_OBJECT_TYPE_MPEG2_LC | AAC_OBJECT_TYPE_MPEG4_LC,
		A2DP_AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_44100 | AAC_SAMPLING_FREQ_96000)
		.channels = AAC_CHANNELS_1,
		.vbr = 1 };

	cfg_aac = cfg_aac_;
	ck_assert_int_eq(a2dp_select_configuration(&a2dp_aac_source, &cfg_aac, sizeof(cfg_aac)), 0);
	ck_assert_int_eq(cfg_aac.object_type, AAC_OBJECT_TYPE_MPEG4_LC);
	ck_assert_int_eq(A2DP_AAC_GET_FREQUENCY(cfg_aac), AAC_SAMPLING_FREQ_44100);
	ck_assert_int_eq(cfg_aac.channels, AAC_CHANNELS_1);
	ck_assert_int_eq(cfg_aac.vbr, 0);

	cfg_aac = cfg_aac_;
	config.aac_prefer_vbr = true;
	ck_assert_int_eq(a2dp_select_configuration(&a2dp_aac_source, &cfg_aac, sizeof(cfg_aac)), 0);
	ck_assert_int_eq(cfg_aac.vbr, 1);

	cfg_aac = cfg_aac_;
	/* FDK-AAC encoder does not support AAC Long Term Prediction */
	cfg_aac.object_type = AAC_OBJECT_TYPE_MPEG4_LTP;
	ck_assert_int_eq(a2dp_select_configuration(&a2dp_aac_source, &cfg_aac, sizeof(cfg_aac)), -1);

#endif

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_a2dp_codecs_codec_id_from_string);
	tcase_add_test(tc, test_a2dp_codecs_codec_id_to_string);
	tcase_add_test(tc, test_a2dp_codecs_get_canonical_name);

	tcase_add_test(tc, test_a2dp_dir);
	tcase_add_test(tc, test_a2dp_codecs_init);
	tcase_add_test(tc, test_a2dp_codec_cmp);
	tcase_add_test(tc, test_a2dp_sep_cmp);
	tcase_add_test(tc, test_a2dp_codec_lookup);
	tcase_add_test(tc, test_a2dp_get_vendor_codec_id);
	tcase_add_test(tc, test_a2dp_check_configuration);
	tcase_add_test(tc, test_a2dp_check_strerror);
	tcase_add_test(tc, test_a2dp_filter_capabilities);
	tcase_add_test(tc, test_a2dp_select_configuration);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
