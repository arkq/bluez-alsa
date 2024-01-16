/*
 * BlueALSA - a2dpconf.c
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

#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <bluetooth/bluetooth.h>

#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/hex.h"

static uint16_t get_codec(const char *s) {

	char buffer[32] = { 0 };
	char *tmp;

	strncpy(buffer, s, sizeof(buffer) - 1);
	if ((tmp = strchr(buffer, ':')) != NULL)
		tmp[0] = '\0';

	return a2dp_codecs_codec_id_from_string(buffer);
}

static ssize_t get_codec_blob(const char *s, void *dest, size_t n) {

	const char *tmp;
	if ((tmp = strchr(s, ':')) != NULL)
		s = tmp + 1;

	size_t len;
	if ((len = strlen(s)) % 2 != 0) {
		fprintf(stderr, "Invalid blob: Size not a multiple of 2: %zd\n", len);
		return -1;
	}

	if (n == 0)
		return len / 2;

	if (n * 2 < len) {
		fprintf(stderr, "Invalid blob buffer size: %zd < %zd\n", n * 2, len);
		return -1;
	}

	return hex2bin(s, dest, len);
}

static char *bintohex(const void *src, size_t n) {
	char *hex = calloc(1, n * 2 + 1);
	bin2hex(src, hex, n);
	return hex;
}

static int check_blob_size(size_t size, size_t value) {
	if (value == size)
		return 0;
	fprintf(stderr, "Invalid codec blob size: %zd != %zd\n", value, size);
	return -1;
}

static void dump_sbc(const void *blob, size_t size) {
	const a2dp_sbc_t *sbc = blob;
	if (check_blob_size(sizeof(*sbc), size) == -1)
		return;
	printf("SBC <hex:%s> {\n"
			"  sampling-frequency:4 =%s%s%s%s\n"
			"  channel-mode:4 =%s%s%s%s\n"
			"  block-length:4 =%s%s%s%s\n"
			"  sub-bands:2 =%s%s\n"
			"  allocation-method:2 =%s%s\n"
			"  min-bit-pool-value:8 = %u\n"
			"  max-bit-pool-value:8 = %u\n"
			"}\n",
			bintohex(sbc, sizeof(*sbc)),
			sbc->frequency & SBC_SAMPLING_FREQ_48000 ? " 48000" : "",
			sbc->frequency & SBC_SAMPLING_FREQ_44100 ? " 44100" : "",
			sbc->frequency & SBC_SAMPLING_FREQ_32000 ? " 32000" : "",
			sbc->frequency & SBC_SAMPLING_FREQ_16000 ? " 16000" : "",
			sbc->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO ? " JointStereo" : "",
			sbc->channel_mode & SBC_CHANNEL_MODE_STEREO ? " Stereo" : "",
			sbc->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL ? " DualChannel" : "",
			sbc->channel_mode & SBC_CHANNEL_MODE_MONO ? " Mono" : "",
			sbc->block_length & SBC_BLOCK_LENGTH_16 ? " 16" : "",
			sbc->block_length & SBC_BLOCK_LENGTH_12 ? " 12" : "",
			sbc->block_length & SBC_BLOCK_LENGTH_8 ? " 8" : "",
			sbc->block_length & SBC_BLOCK_LENGTH_4 ? " 4" : "",
			sbc->subbands & SBC_SUBBANDS_8 ? " 8" : "",
			sbc->subbands & SBC_SUBBANDS_4 ? " 4" : "",
			sbc->allocation_method & SBC_ALLOCATION_LOUDNESS ? " Loudness" : "",
			sbc->allocation_method & SBC_ALLOCATION_SNR ? " SNR" : "",
			sbc->min_bitpool, sbc->max_bitpool);
}

static void dump_mpeg(const void *blob, size_t size) {
	const a2dp_mpeg_t *mpeg = blob;
	if (check_blob_size(sizeof(*mpeg), size) == -1)
		return;
	printf("MPEG-1,2 Audio <hex:%s> {\n"
			"  layer:3 =%s%s%s\n"
			"  crc:1 = %s\n"
			"  channel-mode:4 =%s%s%s%s\n"
			"  <reserved>:1\n"
			"  media-payload-format:1 = MPF-1%s\n"
			"  sampling-frequency:6 =%s%s%s%s%s%s\n"
			"  vbr:1 = %s\n"
			"  bitrate-index:15 = %#x\n"
			"}\n",
			bintohex(mpeg, sizeof(*mpeg)),
			mpeg->layer & MPEG_LAYER_MP3 ? " MP3" : "",
			mpeg->layer & MPEG_LAYER_MP2 ? " MP2" : "",
			mpeg->layer & MPEG_LAYER_MP1 ? " MP1" : "",
			mpeg->crc ? "true" : "false",
			mpeg->channel_mode & MPEG_CHANNEL_MODE_JOINT_STEREO ? " JointStereo" : "",
			mpeg->channel_mode & MPEG_CHANNEL_MODE_STEREO ? " Stereo" : "",
			mpeg->channel_mode & MPEG_CHANNEL_MODE_DUAL_CHANNEL ? " DualChannel" : "",
			mpeg->channel_mode & MPEG_CHANNEL_MODE_MONO ? " Mono" : "",
			mpeg->mpf ? " MPF-2" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_48000 ? " 48000" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_44100 ? " 44100" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_32000 ? " 32000" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_24000 ? " 24000" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_22050 ? " 22050" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_16000 ? " 16000" : "",
			mpeg->vbr ? "true" : "false",
			A2DP_MPEG_GET_BITRATE(*mpeg));
}

static void dump_aac(const void *blob, size_t size) {
	const a2dp_aac_t *aac = blob;
	if (check_blob_size(sizeof(*aac), size) == -1)
		return;
	const uint16_t aac_frequency = A2DP_AAC_GET_FREQUENCY(*aac);
	printf("MPEG-2,4 AAC <hex:%s> {\n"
			"  object-type:7 =%s%s%s%s%s%s%s\n"
			"  dynamic-range-control:1 = %s\n"
			"  sampling-frequency:12 =%s%s%s%s%s%s%s%s%s%s%s%s\n"
			"  channel-mode:4 =%s%s%s%s\n"
			"  vbr:1 = %s\n"
			"  bitrate:23 = %u\n"
			"}\n",
			bintohex(aac, sizeof(*aac)),
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_ELD2 ? " MPEG4-ELD2" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_HE2 ? " MPEG4-HE2" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_HE ? " MPEG4-HE" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_SCA ? " MPEG4-SCA" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_LTP ? " MPEG4-LTP" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_LC ? " MPEG4-LC" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG2_LC ? " MPEG2-LC" : "",
			aac->drc ? "true" : "false",
			aac_frequency & AAC_SAMPLING_FREQ_96000 ? " 96000" : "",
			aac_frequency & AAC_SAMPLING_FREQ_88200 ? " 88200" : "",
			aac_frequency & AAC_SAMPLING_FREQ_64000 ? " 64000" : "",
			aac_frequency & AAC_SAMPLING_FREQ_48000 ? " 48000" : "",
			aac_frequency & AAC_SAMPLING_FREQ_44100 ? " 44100" : "",
			aac_frequency & AAC_SAMPLING_FREQ_32000 ? " 32000" : "",
			aac_frequency & AAC_SAMPLING_FREQ_24000 ? " 24000" : "",
			aac_frequency & AAC_SAMPLING_FREQ_22050 ? " 22050" : "",
			aac_frequency & AAC_SAMPLING_FREQ_16000 ? " 16000" : "",
			aac_frequency & AAC_SAMPLING_FREQ_12000 ? " 12000" : "",
			aac_frequency & AAC_SAMPLING_FREQ_11025 ? " 11025" : "",
			aac_frequency & AAC_SAMPLING_FREQ_8000 ? " 8000" : "",
			aac->channels & AAC_CHANNELS_8 ? " Surround-7.1" : "",
			aac->channels & AAC_CHANNELS_6 ? " Surround-5.1" : "",
			aac->channels & AAC_CHANNELS_2 ? " Stereo" : "",
			aac->channels & AAC_CHANNELS_1 ? " Mono" : "",
			aac->vbr ? "true" : "false",
			A2DP_AAC_GET_BITRATE(*aac));
}

static void dump_usac(const void *blob, size_t size) {
	const a2dp_usac_t *usac = blob;
	if (check_blob_size(sizeof(*usac), size) == -1)
		return;
	const uint32_t usac_frequency = A2DP_USAC_GET_FREQUENCY(*usac);
	printf("MPEG-D USAC <hex:%s> {\n"
			"  object-type:2 =%s\n"
			"  sampling-frequency:26 =%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n"
			"  channel-mode:4 =%s%s\n"
			"  vbr:1 = %s\n"
			"  bitrate:23 = %u\n"
			"}\n",
			bintohex(usac, sizeof(*usac)),
			usac->object_type & USAC_OBJECT_TYPE_MPEGD_DRC ? " MPEG-D-DRC" : "",
			usac_frequency & USAC_SAMPLING_FREQ_96000 ? " 96000" : "",
			usac_frequency & USAC_SAMPLING_FREQ_88200 ? " 88200" : "",
			usac_frequency & USAC_SAMPLING_FREQ_76800 ? " 76800" : "",
			usac_frequency & USAC_SAMPLING_FREQ_70560 ? " 70560" : "",
			usac_frequency & USAC_SAMPLING_FREQ_64000 ? " 64000" : "",
			usac_frequency & USAC_SAMPLING_FREQ_58800 ? " 58800" : "",
			usac_frequency & USAC_SAMPLING_FREQ_48000 ? " 48000" : "",
			usac_frequency & USAC_SAMPLING_FREQ_44100 ? " 44100" : "",
			usac_frequency & USAC_SAMPLING_FREQ_38400 ? " 38400" : "",
			usac_frequency & USAC_SAMPLING_FREQ_35280 ? " 35280" : "",
			usac_frequency & USAC_SAMPLING_FREQ_32000 ? " 32000" : "",
			usac_frequency & USAC_SAMPLING_FREQ_29400 ? " 29400" : "",
			usac_frequency & USAC_SAMPLING_FREQ_24000 ? " 24000" : "",
			usac_frequency & USAC_SAMPLING_FREQ_22050 ? " 22050" : "",
			usac_frequency & USAC_SAMPLING_FREQ_19200 ? " 19200" : "",
			usac_frequency & USAC_SAMPLING_FREQ_17640 ? " 17640" : "",
			usac_frequency & USAC_SAMPLING_FREQ_16000 ? " 16000" : "",
			usac_frequency & USAC_SAMPLING_FREQ_14700 ? " 14700" : "",
			usac_frequency & USAC_SAMPLING_FREQ_12800 ? " 12800" : "",
			usac_frequency & USAC_SAMPLING_FREQ_12000 ? " 12000" : "",
			usac_frequency & USAC_SAMPLING_FREQ_11760 ? " 11760" : "",
			usac_frequency & USAC_SAMPLING_FREQ_11025 ? " 11025" : "",
			usac_frequency & USAC_SAMPLING_FREQ_9600 ? " 9600" : "",
			usac_frequency & USAC_SAMPLING_FREQ_8820 ? " 8820" : "",
			usac_frequency & USAC_SAMPLING_FREQ_8000 ? " 8000" : "",
			usac_frequency & USAC_SAMPLING_FREQ_7350 ? " 7350" : "",
			usac->channels & USAC_CHANNELS_2 ? " Stereo" : "",
			usac->channels & USAC_CHANNELS_1 ? " Mono" : "",
			usac->vbr ? "true" : "false",
			A2DP_USAC_GET_BITRATE(*usac));
}

static void dump_atrac(const void *blob, size_t size) {
	const a2dp_atrac_t *atrac = blob;
	if (check_blob_size(sizeof(*atrac), size) == -1)
		return;
	printf("ATRAC <hex:%s> {\n"
			"  version:3 = ATRAC%u\n"
			"  channel-mode:3 =%s%s%s\n"
			"  <reserved>:4\n"
			"  sampling-frequency:2 =%s%s\n"
			"  vbr:1 = %s\n"
			"  bitrate-index:19 = %#x\n"
			"  max-sound-unit-length:16 = %u\n"
			"  <reserved>:8\n"
			"}\n",
			bintohex(atrac, sizeof(*atrac)),
			atrac->version,
			atrac->channel_mode & ATRAC_CHANNEL_MODE_JOINT_STEREO ? " JointStereo" : "",
			atrac->channel_mode & ATRAC_CHANNEL_MODE_DUAL_CHANNEL ? " DualChannel" : "",
			atrac->channel_mode & ATRAC_CHANNEL_MODE_MONO ? " Mono" : "",
			atrac->frequency & ATRAC_SAMPLING_FREQ_48000 ? " 48000" : "",
			atrac->frequency & ATRAC_SAMPLING_FREQ_44100 ? " 44100" : "",
			atrac->vbr ? "true" : "false",
			A2DP_ATRAC_GET_BITRATE(*atrac),
			A2DP_ATRAC_GET_MAX_SUL(*atrac));
}

static void printf_vendor(const a2dp_vendor_info_t *info) {
	printf(""
			"  vendor-id:32 = %#x [%s]\n"
			"  vendor-codec-id:16 = %#x\n",
			A2DP_VENDOR_INFO_GET_VENDOR_ID(*info),
			bt_compidtostr(A2DP_VENDOR_INFO_GET_VENDOR_ID(*info)),
			A2DP_VENDOR_INFO_GET_CODEC_ID(*info));
}

static void dump_vendor(const void *blob, size_t size) {
	const a2dp_vendor_info_t *info = blob;
	if (size <= sizeof(*info))
		return;
	const void *data = info + 1;
	size_t data_size = size - sizeof(*info);
	printf("<hex:%s> {\n", bintohex(blob, size));
	printf_vendor(info);
	printf(""
			"  data:%zu = hex:%s\n"
			"}\n",
			data_size * 8,
			bintohex(data, data_size));
}

static void printf_aptx(const a2dp_aptx_t *aptx) {
	printf(""
			"  channel-mode:4 =%s%s%s\n"
			"  sampling-frequency:4 =%s%s%s%s\n",
			aptx->channel_mode & APTX_CHANNEL_MODE_STEREO ? " Stereo" : "",
			aptx->channel_mode & APTX_CHANNEL_MODE_TWS ? " DualChannel" : "",
			aptx->channel_mode & APTX_CHANNEL_MODE_MONO ? " Mono" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_48000 ? " 48000" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_44100 ? " 44100" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_32000 ? " 32000" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_16000 ? " 16000" : "");
}

static void dump_aptx(const void *blob, size_t size) {
	const a2dp_aptx_t *aptx = blob;
	if (check_blob_size(sizeof(*aptx), size) == -1)
		return;
	printf("aptX <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&aptx->info);
	printf_aptx(aptx);
	printf("}\n");
}

static void dump_aptx_tws(const void *blob, size_t size) {
	const a2dp_aptx_t *aptx = blob;
	if (check_blob_size(sizeof(*aptx), size) == -1)
		return;
	printf("aptX-TWS <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&aptx->info);
	printf_aptx(aptx);
	printf("}\n");
}

static void dump_aptx_hd(const void *blob, size_t size) {
	const a2dp_aptx_hd_t *aptx_hd = blob;
	if (check_blob_size(sizeof(*aptx_hd), size) == -1)
		return;
	printf("aptX HD <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&aptx_hd->aptx.info);
	printf_aptx(&aptx_hd->aptx);
	printf(""
			"  <reserved>:32\n"
			"}\n");
}

static void dump_aptx_ll(const void *blob, size_t size) {

	const a2dp_aptx_ll_t *aptx_ll = blob;
	const a2dp_aptx_ll_new_t *aptx_ll_new_caps = blob;

	size_t conf_size = sizeof(*aptx_ll);
	if (size >= sizeof(*aptx_ll) && aptx_ll->has_new_caps)
		conf_size = sizeof(*aptx_ll_new_caps);
	if (check_blob_size(conf_size, size) == -1)
		return;

	printf("aptX LL (Sprint) <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&aptx_ll->aptx.info);
	printf_aptx(&aptx_ll->aptx);
	printf(""
			"  <reserved>:6\n"
			"  has-new-caps:1 = %s\n"
			"  bidirectional-link:1 = %s\n",
			aptx_ll->has_new_caps ? "true" : "false",
			aptx_ll->bidirect_link ? "true" : "false");

	if (aptx_ll->has_new_caps)
		printf(""
				"  <reserved>:8\n"
				"  target-codec-level:16 = %u\n"
				"  initial-codec-level:16 = %u\n"
				"  sra-max-rate:8 = %u\n"
				"  sra-avg-time:8 = %u\n"
				"  good-working-level:16 = %u\n",
				A2DP_APTX_LL_GET_TARGET_CODEC_LEVEL(*aptx_ll_new_caps),
				A2DP_APTX_LL_GET_INITIAL_CODEC_LEVEL(*aptx_ll_new_caps),
				aptx_ll_new_caps->sra_max_rate,
				aptx_ll_new_caps->sra_avg_time,
				A2DP_APTX_LL_GET_GOOD_WORKING_LEVEL(*aptx_ll_new_caps));

	printf("}\n");

}

static void dump_faststream(const void *blob, size_t size) {
	const a2dp_faststream_t *faststream = blob;
	if (check_blob_size(sizeof(*faststream), size) == -1)
		return;
	printf("FastStream <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&faststream->info);
	printf(""
			"  direction:8 =%s%s\n"
			"  sampling-frequency-voice:8 =%s\n"
			"  sampling-frequency-music:8 =%s%s\n"
			"}\n",
			faststream->direction & FASTSTREAM_DIRECTION_MUSIC ? " Music" : "",
			faststream->direction & FASTSTREAM_DIRECTION_VOICE ? " Voice" : "",
			faststream->frequency_voice & FASTSTREAM_SAMPLING_FREQ_VOICE_16000 ? " 16000" : "",
			faststream->frequency_music & FASTSTREAM_SAMPLING_FREQ_MUSIC_48000 ? " 48000" : "",
			faststream->frequency_music & FASTSTREAM_SAMPLING_FREQ_MUSIC_44100 ? " 44100" : "");
}

static void dump_lc3plus(const void *blob, size_t size) {
	const a2dp_lc3plus_t *lc3plus = blob;
	if (check_blob_size(sizeof(*lc3plus), size) == -1)
		return;
	const uint16_t lc3plus_frequency = A2DP_LC3PLUS_GET_FREQUENCY(*lc3plus);
	printf("LC3plus <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&lc3plus->info);
	printf(""
			"  frame-duration:4 =%s%s%s\n"
			"  <reserved>:4\n"
			"  channel-mode:8 =%s%s\n"
			"  sampling-frequency:16 =%s%s\n"
			"}\n",
			lc3plus->frame_duration & LC3PLUS_FRAME_DURATION_025 ? " 2.5ms" : "",
			lc3plus->frame_duration & LC3PLUS_FRAME_DURATION_050 ? " 5ms" : "",
			lc3plus->frame_duration & LC3PLUS_FRAME_DURATION_100 ? " 10ms" : "",
			lc3plus->channels & LC3PLUS_CHANNELS_1 ? " Mono" : "",
			lc3plus->channels & LC3PLUS_CHANNELS_2 ? " Stereo" : "",
			lc3plus_frequency & LC3PLUS_SAMPLING_FREQ_48000 ? " 48000" : "",
			lc3plus_frequency & LC3PLUS_SAMPLING_FREQ_96000 ? " 96000" : "");
}

static void dump_ldac(const void *blob, size_t size) {
	const a2dp_ldac_t *ldac = blob;
	if (check_blob_size(sizeof(*ldac), size) == -1)
		return;
	printf("LDAC <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&ldac->info);
	printf(""
			"  <reserved>:2\n"
			"  sampling-frequency:6 =%s%s%s%s%s%s\n"
			"  <reserved>:5\n"
			"  channel-mode:3 =%s%s%s\n"
			"}\n",
			ldac->frequency & LDAC_SAMPLING_FREQ_192000 ? " 192000" : "",
			ldac->frequency & LDAC_SAMPLING_FREQ_176400 ? " 176400" : "",
			ldac->frequency & LDAC_SAMPLING_FREQ_96000 ? " 96000" : "",
			ldac->frequency & LDAC_SAMPLING_FREQ_88200 ? " 88200" : "",
			ldac->frequency & LDAC_SAMPLING_FREQ_48000 ? " 48000" : "",
			ldac->frequency & LDAC_SAMPLING_FREQ_44100 ? " 44100" : "",
			ldac->channel_mode & LDAC_CHANNEL_MODE_STEREO ? " Stereo" : "",
			ldac->channel_mode & LDAC_CHANNEL_MODE_DUAL ? " DualChannel" : "",
			ldac->channel_mode & LDAC_CHANNEL_MODE_MONO ? " Mono" : "");
}

static int lhdc_get_max_bitrate(unsigned int value) {
	switch (value) {
	case LHDC_MAX_BITRATE_400K:
		return 400;
	case LHDC_MAX_BITRATE_500K:
		return 500;
	case LHDC_MAX_BITRATE_900K:
		return 900;
	default:
		return -1;
	}
}

static void dump_lhdc_v1(const void *blob, size_t size) {
	const a2dp_lhdc_v1_t *lhdc = blob;
	if (check_blob_size(sizeof(*lhdc), size) == -1)
		return;
	printf("LHDC v1 <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&lhdc->info);
	printf(""
			"  <reserved>:1\n"
			"  ch-separation:1 = %s\n"
			"  bit-depth:2 =%s%s\n"
			"  frequency:4 =%s%s%s%s\n"
			"}\n",
			lhdc->ch_separation ? "true" : "false",
			lhdc->bit_depth & LHDC_BIT_DEPTH_24 ? " 24" : "",
			lhdc->bit_depth & LHDC_BIT_DEPTH_16 ? " 16" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_96000 ? " 96000" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_88200 ? " 88200" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_48000 ? " 48000" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_44100 ? " 44100" : "");
}

static void dump_lhdc_v2(const void *blob, size_t size) {
	const a2dp_lhdc_v2_t *lhdc = blob;
	if (check_blob_size(sizeof(*lhdc), size) == -1)
		return;
	printf("LHDC v2 <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&lhdc->info);
	printf(""
			"  <reserved>:2\n"
			"  bit-depth:2 =%s%s\n"
			"  frequency:4 =%s%s%s%s\n"
			"  low-latency:1 = %s\n"
			"  max-bitrate:3 = %d\n"
			"  version:4 = %u\n"
			"  <reserved>:4\n"
			"  ch-split-mode:4 =%s%s%s\n"
			"}\n",
			lhdc->bit_depth & LHDC_BIT_DEPTH_24 ? " 24" : "",
			lhdc->bit_depth & LHDC_BIT_DEPTH_16 ? " 16" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_96000 ? " 96000" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_88200 ? " 88200" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_48000 ? " 48000" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_44100 ? " 44100" : "",
			lhdc->low_latency ? "true" : "false",
			lhdc_get_max_bitrate(lhdc->max_bitrate),
			lhdc->version,
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_TWS_PLUS ? " TWS+" : "",
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_TWS ? " TWS" : "",
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_NONE ? " None" : "");
}

static void dump_lhdc_v3(const void *blob, size_t size) {
	const a2dp_lhdc_v3_t *lhdc = blob;
	if (check_blob_size(sizeof(*lhdc), size) == -1)
		return;
	printf("LHDC v3 <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&lhdc->info);
	printf(""
			"  ar:1 = %s\n"
			"  jas:1 = %s\n"
			"  bit-depth:2 =%s%s\n"
			"  frequency:4 =%s%s%s%s\n"
			"  llac:1 = %s\n"
			"  low-latency:1 = %s\n"
			"  max-bitrate:2 = %d\n"
			"  version:4 = %u\n"
			"  lhdc-v4:1 = %s\n"
			"  larc:1 = %s\n"
			"  min-bitrate:1 = %s\n"
			"  meta:1 = %s\n"
			"  ch-split-mode:4 =%s%s%s\n"
			"}\n",
			lhdc->ar ? "true" : "false",
			lhdc->jas ? "true" : "false",
			lhdc->bit_depth & LHDC_BIT_DEPTH_24 ? " 24" : "",
			lhdc->bit_depth & LHDC_BIT_DEPTH_16 ? " 16" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_96000 ? " 96000" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_88200 ? " 88200" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_48000 ? " 48000" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_44100 ? " 44100" : "",
			lhdc->llac ? "true" : "false",
			lhdc->low_latency ? "true" : "false",
			lhdc_get_max_bitrate(lhdc->max_bitrate),
			lhdc->version,
			lhdc->lhdc_v4 ? "true" : "false",
			lhdc->larc ? "true" : "false",
			lhdc->min_bitrate ? "true" : "false",
			lhdc->meta ? "true" : "false",
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_TWS_PLUS ? " TWS+" : "",
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_TWS ? " TWS" : "",
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_NONE ? " None" : "");
}

static void dump_lhdc_v5(const void *blob, size_t size) {
	const a2dp_lhdc_v5_t *lhdc = blob;
	if (check_blob_size(sizeof(*lhdc), size) == -1)
		return;
	printf("LHDC v5 <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&lhdc->info);
	printf(""
			"  data:%zu = hex:%s\n"
			"}\n",
			sizeof(*lhdc) - sizeof(lhdc->info),
			bintohex(blob + sizeof(lhdc->info), sizeof(*lhdc) - sizeof(lhdc->info)));
}

static void dump_opus(const void *blob, size_t size) {
	const a2dp_opus_t *opus = blob;
	if (check_blob_size(sizeof(*opus), size) == -1)
		return;
	printf("Opus <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&opus->info);
	printf(""
			"  music-channels:8 = %u\n"
			"  music-coupled-streams:8 = %u\n"
			"  music-location32: = %#x\n"
			"  music-frame-duration:8 =%s%s%s%s%s\n"
			"  music-bitrate:16 = %u\n"
			"  voice-channels:8 = %u\n"
			"  voice-coupled-streams:8 = %u\n"
			"  voice-location32: = %#x\n"
			"  voice-frame-duration:8 =%s%s%s%s%s\n"
			"  voice-bitrate:16 = %u\n"
			"}\n",
			opus->music.channels,
			opus->music.coupled_streams,
			A2DP_OPUS_GET_LOCATION(opus->music),
			opus->music.frame_duration & OPUS_FRAME_DURATION_025 ? " 2.5ms" : "",
			opus->music.frame_duration & OPUS_FRAME_DURATION_050 ? " 5ms" : "",
			opus->music.frame_duration & OPUS_FRAME_DURATION_100 ? " 10ms" : "",
			opus->music.frame_duration & OPUS_FRAME_DURATION_200 ? " 20ms" : "",
			opus->music.frame_duration & OPUS_FRAME_DURATION_400 ? " 40ms" : "",
			A2DP_OPUS_GET_BITRATE(opus->music) * 1024,
			opus->voice.channels,
			opus->voice.coupled_streams,
			A2DP_OPUS_GET_LOCATION(opus->voice),
			opus->voice.frame_duration & OPUS_FRAME_DURATION_025 ? " 2.5ms" : "",
			opus->voice.frame_duration & OPUS_FRAME_DURATION_050 ? " 5ms" : "",
			opus->voice.frame_duration & OPUS_FRAME_DURATION_100 ? " 10ms" : "",
			opus->voice.frame_duration & OPUS_FRAME_DURATION_200 ? " 20ms" : "",
			opus->voice.frame_duration & OPUS_FRAME_DURATION_400 ? " 40ms" : "",
			A2DP_OPUS_GET_BITRATE(opus->voice) * 1024);
}

static struct {
	uint16_t codec_id;
	size_t blob_size;
	void (*dump)(const void *, size_t);
} dumps[] = {
	{ A2DP_CODEC_SBC, sizeof(a2dp_sbc_t), dump_sbc },
	{ A2DP_CODEC_MPEG12, sizeof(a2dp_mpeg_t), dump_mpeg },
	{ A2DP_CODEC_MPEG24, sizeof(a2dp_aac_t), dump_aac },
	{ A2DP_CODEC_MPEGD, sizeof(a2dp_usac_t), dump_usac },
	{ A2DP_CODEC_ATRAC, sizeof(a2dp_atrac_t), dump_atrac },
	{ A2DP_CODEC_VENDOR_APTX, sizeof(a2dp_aptx_t), dump_aptx },
	{ A2DP_CODEC_VENDOR_APTX_TWS, sizeof(a2dp_aptx_t), dump_aptx_tws },
	{ A2DP_CODEC_VENDOR_APTX_AD, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_APTX_HD, sizeof(a2dp_aptx_hd_t), dump_aptx_hd },
	{ A2DP_CODEC_VENDOR_APTX_LL, sizeof(a2dp_aptx_ll_t), dump_aptx_ll },
	{ A2DP_CODEC_VENDOR_APTX_LL, sizeof(a2dp_aptx_ll_new_t), dump_aptx_ll },
	{ A2DP_CODEC_VENDOR_FASTSTREAM, sizeof(a2dp_faststream_t), dump_faststream },
	{ A2DP_CODEC_VENDOR_LC3PLUS, sizeof(a2dp_lc3plus_t), dump_lc3plus },
	{ A2DP_CODEC_VENDOR_LDAC, sizeof(a2dp_ldac_t), dump_ldac },
	{ A2DP_CODEC_VENDOR_LHDC_V1, sizeof(a2dp_lhdc_v1_t), dump_lhdc_v1 },
	{ A2DP_CODEC_VENDOR_LHDC_V2, sizeof(a2dp_lhdc_v2_t), dump_lhdc_v2 },
	{ A2DP_CODEC_VENDOR_LHDC_V3, sizeof(a2dp_lhdc_v3_t), dump_lhdc_v3 },
	{ A2DP_CODEC_VENDOR_LHDC_V5, sizeof(a2dp_lhdc_v5_t), dump_lhdc_v5 },
	{ A2DP_CODEC_VENDOR_LHDC_LL, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_OPUS, sizeof(a2dp_opus_t), dump_opus },
	{ A2DP_CODEC_VENDOR_SAMSUNG_HD, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_SAMSUNG_SC, -1, dump_vendor },
};

int dump(const char *config, bool detect) {

	uint16_t codec_id = get_codec(config);
	int rv = -1;

	ssize_t blob_size;
	if ((blob_size = get_codec_blob(config, NULL, 0)) == -1)
		return -1;

	void *blob = malloc(blob_size);
	if (get_codec_blob(config, blob, blob_size) == -1)
		goto final;

	rv = 0;
	for (size_t i = 0; i < ARRAYSIZE(dumps); i++)
		if (dumps[i].codec_id == codec_id) {
			dumps[i].dump(blob, blob_size);
			goto final;
		}

	if (detect) {
		for (size_t i = 0; i < ARRAYSIZE(dumps); i++)
			if (dumps[i].blob_size == (size_t)blob_size)
				dumps[i].dump(blob, blob_size);
		dump_vendor(blob, blob_size);
		goto final;
	}

	fprintf(stderr, "Couldn't detect codec type: %s\n", config);
	rv = -1;

final:
	free(blob);
	return rv;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVx";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "auto-detect", no_argument, NULL, 'x' },
		{ 0, 0, 0, 0 },
	};

	int rv = EXIT_SUCCESS;
	bool detect = false;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <CONFIG>...\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -x, --auto-detect\ttry to auto-detect codec\n"
					"\nExamples:\n"
					"  %s sbc:ffff0235\n"
					"  %s aptx:4f0000000100ff\n",
					argv[0], argv[0], argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'x' /* --auto-detect */ :
			detect = true;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (argc - optind < 1)
		goto usage;

	int i;
	for (i = optind; i < argc; i++)
		if (dump(argv[i], detect) == -1)
			rv = EXIT_FAILURE;

	return rv;
}
