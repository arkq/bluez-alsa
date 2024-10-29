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
#include "a2dp-faststream.h"
#include "a2dp-sbc.h"
#include "ba-config.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "codec-sbc.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

#include "inc/check.inc"

const char *ba_transport_debug_name(const struct ba_transport *t) { (void)t; return "x"; }
uint32_t ba_transport_get_codec(const struct ba_transport *t) { (void)t; return 0; }
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
int ba_transport_pcm_delay_sync(struct ba_transport_pcm *pcm, unsigned int update_mask) {
	(void)pcm; (void)update_mask; return -1; }

CK_START_TEST(test_a2dp_codecs_codec_id_from_string) {
	ck_assert_uint_eq(a2dp_codecs_codec_id_from_string("SBC"), A2DP_CODEC_SBC);
	ck_assert_uint_eq(a2dp_codecs_codec_id_from_string("apt-x"),
			A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID));
	ck_assert_uint_eq(a2dp_codecs_codec_id_from_string("unknown"), 0xFFFFFFFF);
} CK_END_TEST

CK_START_TEST(test_a2dp_codecs_codec_id_to_string) {
	ck_assert_str_eq(a2dp_codecs_codec_id_to_string(A2DP_CODEC_SBC), "SBC");
	const uint32_t vendor_codec_id = A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID);
	ck_assert_str_eq(a2dp_codecs_codec_id_to_string(vendor_codec_id), "aptX");
	ck_assert_ptr_eq(a2dp_codecs_codec_id_to_string(0xFFFFFFFF), NULL);
} CK_END_TEST

CK_START_TEST(test_a2dp_codecs_get_canonical_name) {
	ck_assert_str_eq(a2dp_codecs_get_canonical_name("apt-x"), "aptX");
	ck_assert_str_eq(a2dp_codecs_get_canonical_name("Foo-Bar"), "Foo-Bar");
} CK_END_TEST

CK_START_TEST(test_a2dp_type) {
	ck_assert_int_eq(A2DP_SOURCE, !A2DP_SINK);
	ck_assert_int_eq(!A2DP_SOURCE, A2DP_SINK);
} CK_END_TEST

CK_START_TEST(test_a2dp_seps_init) {
	a2dp_seps_init();
} CK_END_TEST

CK_START_TEST(test_a2dp_sep_ptr_cmp) {

	struct a2dp_sep c1 = { .config = { .type = A2DP_SOURCE, .codec_id = A2DP_CODEC_SBC } };
	struct a2dp_sep c2 = { .config = { .type = A2DP_SOURCE, .codec_id = A2DP_CODEC_MPEG24 } };
	struct a2dp_sep c3 = { .config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID) } };
	struct a2dp_sep c4 = { .config = { .type = A2DP_SINK, .codec_id = A2DP_CODEC_SBC } };
	struct a2dp_sep c5 = { .config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID) } };
	struct a2dp_sep c6 = { .config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID) } };
	struct a2dp_sep c7 = { .config = { .type = A2DP_SINK, .codec_id = 0xFFFFFFFF } };

	struct a2dp_sep * codecs[] = { &c3, &c1, &c6, &c4, &c7, &c5, &c2 };
	qsort(codecs, ARRAYSIZE(codecs), sizeof(*codecs), QSORT_COMPAR(a2dp_sep_ptr_cmp));

	ck_assert_ptr_eq(codecs[0], &c1);
	ck_assert_ptr_eq(codecs[1], &c2);
	ck_assert_ptr_eq(codecs[2], &c3);
	ck_assert_ptr_eq(codecs[3], &c4);
	ck_assert_ptr_eq(codecs[4], &c5);
	ck_assert_ptr_eq(codecs[5], &c6);
	ck_assert_ptr_eq(codecs[6], &c7);

} CK_END_TEST

CK_START_TEST(test_a2dp_sep_lookup) {
	ck_assert_ptr_eq(a2dp_sep_lookup(A2DP_SOURCE, A2DP_CODEC_SBC), &a2dp_sbc_source);
	ck_assert_ptr_eq(a2dp_sep_lookup(A2DP_SOURCE, 0xFFFFFFFF), NULL);
} CK_END_TEST

CK_START_TEST(test_a2dp_get_vendor_codec_id) {

	uint8_t cfg0[4] = { 0xDE, 0xAD, 0xB0, 0xBE };
	ck_assert_int_eq(a2dp_get_vendor_codec_id(cfg0, sizeof(cfg0)), 0xFFFFFFFF);
	ck_assert_int_eq(errno, EINVAL);

	a2dp_aptx_t cfg1 = { A2DP_VENDOR_INFO_INIT(APTX_VENDOR_ID, APTX_CODEC_ID), 0, 0 };
	ck_assert_int_eq(
			a2dp_get_vendor_codec_id(&cfg1, sizeof(cfg1)),
			A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID));

} CK_END_TEST

CK_START_TEST(test_a2dp_check_configuration) {

	const a2dp_sbc_t cfg_valid = {
		.sampling_freq = SBC_SAMPLING_FREQ_44100,
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
		.sampling_freq = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_STEREO | SBC_CHANNEL_MODE_JOINT_STEREO,
		.block_length = SBC_BLOCK_LENGTH_8,
		.allocation_method = SBC_ALLOCATION_SNR,
	};

	ck_assert_int_eq(a2dp_check_configuration(&a2dp_sbc_source,
				&cfg_invalid, sizeof(cfg_invalid)), A2DP_CHECK_ERR_RATE);

#if ENABLE_AAC
	a2dp_aac_t cfg_aac_invalid = {
		/* FDK-AAC encoder does not support AAC Long Term Prediction */
		.object_type = AAC_OBJECT_TYPE_MPEG4_LTP,
		A2DP_AAC_INIT_SAMPLING_FREQ(AAC_SAMPLING_FREQ_44100)
		.channel_mode = AAC_CHANNEL_MODE_MONO };
	ck_assert_int_eq(a2dp_check_configuration(&a2dp_aac_source,
			&cfg_aac_invalid, sizeof(cfg_aac_invalid)), A2DP_CHECK_ERR_OBJECT_TYPE);
#endif

#if ENABLE_FASTSTREAM

	a2dp_faststream_t cfg_faststream = {
		.info = A2DP_VENDOR_INFO_INIT(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID) };

	/* FastStream codec requires at least one direction to be set. */
	ck_assert_int_eq(a2dp_check_configuration(&a2dp_faststream_source,
			&cfg_faststream, sizeof(cfg_faststream)), A2DP_CHECK_ERR_DIRECTIONS);

	/* Check for valid unidirectional configuration. */
	cfg_faststream.direction |= FASTSTREAM_DIRECTION_MUSIC;
	cfg_faststream.sampling_freq_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_44100;
	ck_assert_int_eq(a2dp_check_configuration(&a2dp_faststream_source,
			&cfg_faststream, sizeof(cfg_faststream)), A2DP_CHECK_OK);

	/* Check for valid bidirectional configuration. */
	cfg_faststream.direction |= FASTSTREAM_DIRECTION_VOICE;
	cfg_faststream.sampling_freq_voice = FASTSTREAM_SAMPLING_FREQ_VOICE_16000;
	ck_assert_int_eq(a2dp_check_configuration(&a2dp_faststream_source,
			&cfg_faststream, sizeof(cfg_faststream)), A2DP_CHECK_OK);

#endif

} CK_END_TEST

CK_START_TEST(test_a2dp_check_strerror) {
	ck_assert_str_eq(a2dp_check_strerror(A2DP_CHECK_ERR_SIZE), "Invalid size");
	ck_assert_str_eq(a2dp_check_strerror(0xFFFF), "Check error");
} CK_END_TEST

CK_START_TEST(test_a2dp_caps) {

	struct a2dp_sep * const * seps = a2dp_seps;
	for (const struct a2dp_sep *sep = *seps; sep != NULL; sep = *++seps) {
		debug("%s", sep->name);

		/* Check whether all capability helpers are set. */

		ck_assert_ptr_ne(sep->caps_helpers, NULL);
		ck_assert_ptr_ne(sep->caps_helpers->intersect, NULL);
		ck_assert_ptr_ne(sep->caps_helpers->has_stream, NULL);
		ck_assert_ptr_ne(sep->caps_helpers->foreach_channel_mode, NULL);
		ck_assert_ptr_ne(sep->caps_helpers->foreach_sample_rate, NULL);
		ck_assert_ptr_ne(sep->caps_helpers->select_channel_mode, NULL);
		ck_assert_ptr_ne(sep->caps_helpers->select_sample_rate, NULL);

		/* Run smoke tests for all capability helpers. */

		a2dp_t caps = sep->config.capabilities;
		sep->caps_helpers->intersect(&caps, &sep->config.capabilities);

		/* All our SEPs shall support the MAIN stream. The BACKCHANNEL
		 * stream is optional, though. */

		ck_assert_uint_eq(sep->caps_helpers->has_stream(&caps, A2DP_MAIN), true);
		sep->caps_helpers->has_stream(&caps, A2DP_BACKCHANNEL);

		unsigned int channel_mode = 0;
		sep->caps_helpers->foreach_channel_mode(&caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode);
		sep->caps_helpers->foreach_channel_mode(&caps, A2DP_BACKCHANNEL,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode);

		unsigned int sampling_freq = 0;
		sep->caps_helpers->foreach_sample_rate(&caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq);
		sep->caps_helpers->foreach_sample_rate(&caps, A2DP_BACKCHANNEL,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq);

		sep->caps_helpers->select_channel_mode(&caps, A2DP_MAIN, 2);
		sep->caps_helpers->select_channel_mode(&caps, A2DP_BACKCHANNEL, 1);

		sep->caps_helpers->select_sample_rate(&caps, A2DP_MAIN, 48000);
		sep->caps_helpers->select_sample_rate(&caps, A2DP_BACKCHANNEL, 16000);

	}

} CK_END_TEST

CK_START_TEST(test_a2dp_caps_intersect) {

	a2dp_sbc_t caps_sbc = {
		.sampling_freq = SBC_SAMPLING_FREQ_44100,
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
		.sampling_freq = APTX_SAMPLING_FREQ_32000 | APTX_SAMPLING_FREQ_44100,
		.channel_mode = APTX_CHANNEL_MODE_MONO | APTX_CHANNEL_MODE_STEREO,
	};
#endif

	hexdump("Capabilities A", &caps_sbc, sizeof(caps_sbc));
	hexdump("Capabilities B", &a2dp_sbc_source.config.capabilities, sizeof(caps_sbc));
	a2dp_sbc_source.caps_helpers->intersect(&caps_sbc, &a2dp_sbc_source.config.capabilities);

	hexdump("Intersection", &caps_sbc, sizeof(caps_sbc));
	ck_assert_int_eq(caps_sbc.sampling_freq, SBC_SAMPLING_FREQ_44100);
	ck_assert_int_eq(caps_sbc.channel_mode, SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_STEREO);
	ck_assert_int_eq(caps_sbc.block_length, SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8);
	ck_assert_int_eq(caps_sbc.subbands, SBC_SUBBANDS_4);
	ck_assert_int_eq(caps_sbc.allocation_method, SBC_ALLOCATION_SNR);
	ck_assert_int_eq(caps_sbc.min_bitpool, MAX(SBC_MIN_BITPOOL, 42));
	ck_assert_int_eq(caps_sbc.max_bitpool, MIN(SBC_MAX_BITPOOL, 255));

#if ENABLE_APTX
	/* Check whether generic bitwise AND intersection works correctly. */
	hexdump("Capabilities A", &caps_aptx, sizeof(caps_aptx));
	hexdump("Capabilities B", &a2dp_aptx_source.config.capabilities, sizeof(caps_aptx));
	a2dp_aptx_source.caps_helpers->intersect(&caps_aptx, &a2dp_aptx_source.config.capabilities);
	hexdump("Intersection", &caps_aptx, sizeof(caps_aptx));
	ck_assert_int_eq(caps_aptx.sampling_freq, APTX_SAMPLING_FREQ_32000 | APTX_SAMPLING_FREQ_44100);
	ck_assert_int_eq(caps_aptx.channel_mode, APTX_CHANNEL_MODE_STEREO);
#endif

} CK_END_TEST

CK_START_TEST(test_a2dp_caps_foreach_get_best) {

	a2dp_sbc_t caps_sbc = {
		.sampling_freq = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_STEREO,
	};

	unsigned int channel_mode = 0;
	ck_assert_int_eq(a2dp_sbc_source.caps_helpers->foreach_channel_mode(&caps_sbc,
			A2DP_MAIN, a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode), 0);
	ck_assert_uint_eq(channel_mode, SBC_CHANNEL_MODE_STEREO);

	unsigned int sampling_freq = 0;
	ck_assert_int_eq(a2dp_sbc_source.caps_helpers->foreach_sample_rate(&caps_sbc,
			A2DP_MAIN, a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq), 0);
	ck_assert_uint_eq(sampling_freq, SBC_SAMPLING_FREQ_44100);

#if ENABLE_AAC

	/* Check default internal limits for selecting number of channels (up to
	 * 2 channels) and sample rate (up to 48 kHz). */

	a2dp_aac_t caps_aac = {
		A2DP_AAC_INIT_SAMPLING_FREQ(AAC_SAMPLING_FREQ_48000 | AAC_SAMPLING_FREQ_96000)
		.channel_mode = AAC_CHANNEL_MODE_MONO | AAC_CHANNEL_MODE_STEREO | AAC_CHANNEL_MODE_5_1,
	};

	channel_mode = 0;
	ck_assert_int_eq(a2dp_aac_source.caps_helpers->foreach_channel_mode(&caps_aac,
			A2DP_MAIN, a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode), 1);
	ck_assert_uint_eq(channel_mode, AAC_CHANNEL_MODE_STEREO);

	sampling_freq = 0;
	ck_assert_int_eq(a2dp_aac_source.caps_helpers->foreach_sample_rate(&caps_aac,
			A2DP_MAIN, a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq), 1);
	ck_assert_uint_eq(sampling_freq, AAC_SAMPLING_FREQ_48000);

#endif

} CK_END_TEST

CK_START_TEST(test_a2dp_caps_select_channels_and_sampling) {

	a2dp_sbc_t caps_sbc = {
		.sampling_freq = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL | SBC_CHANNEL_MODE_STEREO,
	};

	a2dp_sbc_source.caps_helpers->select_channel_mode(&caps_sbc, A2DP_MAIN, 2);
	ck_assert_uint_eq(caps_sbc.channel_mode, SBC_CHANNEL_MODE_STEREO);

	a2dp_sbc_source.caps_helpers->select_sample_rate(&caps_sbc, A2DP_MAIN, 16000);
	ck_assert_uint_eq(caps_sbc.sampling_freq, SBC_SAMPLING_FREQ_16000);

} CK_END_TEST

CK_START_TEST(test_a2dp_select_configuration) {

	a2dp_sbc_t cfg;
	const a2dp_sbc_t cfg_ = {
		.sampling_freq = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_44100 | SBC_SAMPLING_FREQ_48000,
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
	ck_assert_int_eq(cfg.sampling_freq, SBC_SAMPLING_FREQ_48000);
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
	ck_assert_int_eq(cfg.sampling_freq, SBC_SAMPLING_FREQ_44100);
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
		A2DP_AAC_INIT_SAMPLING_FREQ(AAC_SAMPLING_FREQ_44100 | AAC_SAMPLING_FREQ_96000)
		.channel_mode = AAC_CHANNEL_MODE_MONO,
		.vbr = 1 };

	cfg_aac = cfg_aac_;
	ck_assert_int_eq(a2dp_select_configuration(&a2dp_aac_source, &cfg_aac, sizeof(cfg_aac)), 0);
	ck_assert_int_eq(cfg_aac.object_type, AAC_OBJECT_TYPE_MPEG4_LC);
	ck_assert_int_eq(A2DP_AAC_GET_SAMPLING_FREQ(cfg_aac), AAC_SAMPLING_FREQ_44100);
	ck_assert_int_eq(cfg_aac.channel_mode, AAC_CHANNEL_MODE_MONO);
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

	tcase_add_test(tc, test_a2dp_type);
	tcase_add_test(tc, test_a2dp_seps_init);
	tcase_add_test(tc, test_a2dp_sep_ptr_cmp);
	tcase_add_test(tc, test_a2dp_sep_lookup);
	tcase_add_test(tc, test_a2dp_get_vendor_codec_id);

	tcase_add_test(tc, test_a2dp_caps);
	tcase_add_test(tc, test_a2dp_caps_intersect);
	tcase_add_test(tc, test_a2dp_caps_foreach_get_best);
	tcase_add_test(tc, test_a2dp_caps_select_channels_and_sampling);

	tcase_add_test(tc, test_a2dp_check_configuration);
	tcase_add_test(tc, test_a2dp_check_strerror);
	tcase_add_test(tc, test_a2dp_select_configuration);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
