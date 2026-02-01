/*
 * test-codec-aptx.c
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>
#include <stddef.h>

#include <check.h>

#include "codec-aptx.h"
#include "shared/defs.h"

#include "inc/check.inc"

#if ENABLE_APTX
CK_START_TEST(test_codec_aptx_encode) {

	const int16_t pcm[8] = { 0, 10, 100, 1000, 1000, 100, 10, 0 };
	uint8_t out[8];
	size_t len;

	HANDLE_APTX handle;
	ck_assert_ptr_nonnull(handle = aptxenc_init());

	len = sizeof(out);
	/* Check too short input buffer. */
	ck_assert_int_eq(aptxenc_encode(handle, pcm, ARRAYSIZE(pcm) - 2, out, &len), -1);

	len = 2;
	/* Check too short output buffer. */
	ck_assert_int_eq(aptxenc_encode(handle, pcm, ARRAYSIZE(pcm), out, &len), -1);

	len = sizeof(out);
	/* Check proper encoding. */
	ck_assert_int_eq(aptxenc_encode(handle, pcm, ARRAYSIZE(pcm), out, &len), 8);
	const uint8_t expected[4] = { 0x4B, 0xBF, 0x4B, 0xBF };
	ck_assert_mem_eq(out, expected, sizeof(expected));
	ck_assert_int_eq(len, 4);

	aptxenc_destroy(handle);

} CK_END_TEST
#endif

#if ENABLE_APTX && HAVE_APTX_DECODE
CK_START_TEST(test_codec_aptx_decode) {

	const uint8_t enc[4] = { 0x4B, 0xBF, 0x4B, 0xBF };
	int16_t out[16];
	size_t samples;

	HANDLE_APTX handle;
	ck_assert_ptr_nonnull(handle = aptxdec_init());

	samples = sizeof(out);
	/* Check too short input buffer. */
	ck_assert_int_eq(aptxdec_decode(handle, enc, sizeof(enc) - 2, out, &samples), -1);

	samples = 2;
	/* Check too short output buffer. */
	ck_assert_int_eq(aptxdec_decode(handle, enc, sizeof(enc), out, &samples), -1);

	samples = sizeof(out);
	/* Check proper decoding. */
	ck_assert_int_eq(aptxdec_decode(handle, enc, sizeof(enc), out, &samples), 4);
	/* Initial sync-word decoding does not output any samples. */
	ck_assert_int_eq(samples, 0);

	aptxdec_destroy(handle);

} CK_END_TEST
#endif

#if ENABLE_APTX_HD
CK_START_TEST(test_codec_aptx_hd_encode) {

	const int32_t pcm[8] = { 0, 10, 100, 1000, 1000, 100, 10, 0 };
	uint8_t out[8];
	size_t len;

	HANDLE_APTX handle;
	ck_assert_ptr_nonnull(handle = aptxhdenc_init());

	len = sizeof(out);
	/* Check too short input buffer. */
	ck_assert_int_eq(aptxhdenc_encode(handle, pcm, ARRAYSIZE(pcm) - 2, out, &len), -1);

	len = 2;
	/* Check too short output buffer. */
	ck_assert_int_eq(aptxhdenc_encode(handle, pcm, ARRAYSIZE(pcm), out, &len), -1);

	len = sizeof(out);
	/* Check proper encoding. */
	ck_assert_int_eq(aptxhdenc_encode(handle, pcm, ARRAYSIZE(pcm), out, &len), 8);
	const uint8_t expected[6] = { 0x73, 0xBE, 0xFF, 0x73, 0xBE, 0xFF };
	ck_assert_mem_eq(out, expected, sizeof(expected));
	ck_assert_int_eq(len, 6);

	aptxhdenc_destroy(handle);

} CK_END_TEST
#endif

#if ENABLE_APTX_HD && HAVE_APTX_HD_DECODE
CK_START_TEST(test_codec_aptx_hd_decode) {

	const uint8_t enc[6] = { 0x73, 0xBE, 0xFF, 0x73, 0xBE, 0xFF };
	int32_t out[16];
	size_t samples;

	HANDLE_APTX handle;
	ck_assert_ptr_nonnull(handle = aptxhddec_init());

	samples = sizeof(out);
	/* Check too short input buffer. */
	ck_assert_int_eq(aptxhddec_decode(handle, enc, sizeof(enc) - 2, out, &samples), -1);

	samples = 2;
	/* Check too short output buffer. */
	ck_assert_int_eq(aptxhddec_decode(handle, enc, sizeof(enc), out, &samples), -1);

	samples = sizeof(out);
	/* Check proper decoding. */
	ck_assert_int_eq(aptxhddec_decode(handle, enc, sizeof(enc), out, &samples), 6);
	/* Initial sync-word decoding does not output any samples. */
	ck_assert_int_eq(samples, 0);

	aptxhddec_destroy(handle);

} CK_END_TEST
#endif

int main(void) {

	Suite * s = suite_create(__FILE__);
	TCase * tc = tcase_create(__FILE__);
	SRunner * sr = srunner_create(s);

	suite_add_tcase(s, tc);

#if ENABLE_APTX
	tcase_add_test(tc, test_codec_aptx_encode);
#endif
#if ENABLE_APTX && HAVE_APTX_DECODE
	tcase_add_test(tc, test_codec_aptx_decode);
#endif
#if ENABLE_APTX_HD
	tcase_add_test(tc, test_codec_aptx_hd_encode);
#endif
#if ENABLE_APTX_HD && HAVE_APTX_HD_DECODE
	tcase_add_test(tc, test_codec_aptx_hd_decode);
#endif

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
