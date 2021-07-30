/*
 * BlueALSA - a2dpconf.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
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
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include <bluetooth/bluetooth.h>

#include "a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/hex.h"

static const struct {
	uint16_t codec_id;
	const char *name[5];
} codecs[] = {
	{ A2DP_CODEC_SBC, { "SBC" } },
	{ A2DP_CODEC_MPEG12, { "MPEG", "MPEG12", "MP3"} },
	{ A2DP_CODEC_MPEG24, { "MPEG24", "AAC" } },
	{ A2DP_CODEC_ATRAC, { "ATRAC" } },
	{ A2DP_CODEC_VENDOR_APTX, { "aptX" } },
	{ A2DP_CODEC_VENDOR_APTX_AD, { "aptX-AD" } },
	{ A2DP_CODEC_VENDOR_APTX_HD, { "aptX-HD" } },
	{ A2DP_CODEC_VENDOR_APTX_LL, { "aptX-LL" } },
	{ A2DP_CODEC_VENDOR_APTX_TWS, { "aptX-TWS" } },
	{ A2DP_CODEC_VENDOR_FASTSTREAM, { "FastStream", "FS" } },
	{ A2DP_CODEC_VENDOR_LDAC, { "LDAC" } },
	{ A2DP_CODEC_VENDOR_LHDC, { "LHDC" } },
	{ A2DP_CODEC_VENDOR_LHDC_LL, { "LHDC-LL", "LLAC" } },
	{ A2DP_CODEC_VENDOR_LHDC_V1, { "LHDC-v1" } },
	{ A2DP_CODEC_VENDOR_SAMSUNG_HD, { "samsung-HD" } },
	{ A2DP_CODEC_VENDOR_SAMSUNG_SC, { "samsung-SC" } },
};

static uint16_t get_codec(const char *s) {

	const char *tmp;
	size_t len = strlen(s);
	if ((tmp = strchr(s, ':')) != NULL)
		len = tmp - s;

	if (len == 0)
		goto fail;

	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		for (size_t n = 0; n < ARRAYSIZE(codecs[i].name); n++)
			if (codecs[i].name[n] != NULL &&
					strlen(codecs[i].name[n]) == len &&
					strncasecmp(s, codecs[i].name[n], len) == 0)
				return codecs[i].codec_id;

fail:
	return 0xFFFF;
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
			"  subbands:2 =%s%s\n"
			"  allocation-method:2 =%s%s\n"
			"  min-bitpool-value:8 = %u\n"
			"  max-bitpool-value:8 = %u\n"
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
			"  mpf:1 = %s\n"
			"  sampling-frequency:6 =%s%s%s%s%s%s\n"
			"  vbr:1 = %s\n"
			"  bit-rate-index:15 = %#x\n"
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
			mpeg->mpf ? "true" : "false",
			mpeg->frequency & MPEG_SAMPLING_FREQ_48000 ? " 48000" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_44100 ? " 44100" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_32000 ? " 32000" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_24000 ? " 24000" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_22050 ? " 22050" : "",
			mpeg->frequency & MPEG_SAMPLING_FREQ_16000 ? " 16000" : "",
			mpeg->vbr ? "true" : "false",
			MPEG_GET_BITRATE(*mpeg));
}

static void dump_aac(const void *blob, size_t size) {
	const a2dp_aac_t *aac = blob;
	if (check_blob_size(sizeof(*aac), size) == -1)
		return;
	printf("MPEG-2,4 AAC <hex:%s> {\n"
			"  object-type:8 =%s%s%s%s\n"
			"  sampling-frequency:12 =%s%s%s%s%s%s%s%s%s%s%s%s\n"
			"  channel-mode:2 =%s%s\n"
			"  <reserved>:2\n"
			"  vbr:1 = %s\n"
			"  bit-rate:23 = %u\n"
			"}\n",
			bintohex(aac, sizeof(*aac)),
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_SCA ? " MPGE4-SCA" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LTP ? " MPGE4-LTP" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC ? " MPGE4-LC" : "",
			aac->object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC ? " MPGE2-LC" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_96000 ? " 96000" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_88200 ? " 88200" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_64000 ? " 64000" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_48000 ? " 48000" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_44100 ? " 44100" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_32000 ? " 32000" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_24000 ? " 24000" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_22050 ? " 22050" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_16000 ? " 16000" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_12000 ? " 12000" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_11025 ? " 11025" : "",
			AAC_GET_FREQUENCY(*aac) & AAC_SAMPLING_FREQ_8000 ? " 8000" : "",
			aac->channels & AAC_CHANNELS_2 ? " Stereo" : "",
			aac->channels & AAC_CHANNELS_1 ? " Mono" : "",
			aac->vbr ? "true" : "false",
			AAC_GET_BITRATE(*aac));
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
			"  bit-rate-index:19 = %#x\n"
			"  max-sul:16 = %u\n"
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
			ATRAC_GET_BITRATE(*atrac),
			ATRAC_GET_MAX_SUL(*atrac));
}

static void printf_vendor(const a2dp_vendor_codec_t *info) {
	printf(""
			"  vendor-id:32 = %#x [%s]\n"
			"  vendor-codec-id:16 = %#x\n",
			A2DP_GET_VENDOR_ID(*info),
			bt_compidtostr(A2DP_GET_VENDOR_ID(*info)),
			A2DP_GET_CODEC_ID(*info));
}

static void dump_vendor(const void *blob, size_t size) {
	const a2dp_vendor_codec_t *info = blob;
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
			"  bidirect-link:1 = %s\n",
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
				APTX_LL_GET_TARGET_CODEC_LEVEL(*aptx_ll_new_caps),
				APTX_LL_GET_INITIAL_CODEC_LEVEL(*aptx_ll_new_caps),
				aptx_ll_new_caps->sra_max_rate,
				aptx_ll_new_caps->sra_avg_time,
				APTX_LL_GET_GOOD_WORKING_LEVEL(*aptx_ll_new_caps));

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

static int lhdc_get_max_bit_rate(const a2dp_lhdc_t *lhdc) {
	if (lhdc->max_bit_rate == LHDC_MAX_BIT_RATE_400K)
		return 400;
	if (lhdc->max_bit_rate == LHDC_MAX_BIT_RATE_500K)
		return 500;
	if (lhdc->max_bit_rate == LHDC_MAX_BIT_RATE_900K)
		return 900;
	return -1;
}

static void dump_lhdc(const void *blob, size_t size) {
	const a2dp_lhdc_t *lhdc = blob;
	if (check_blob_size(sizeof(*lhdc), size) == -1)
		return;
	printf("LHDC <hex:%s> {\n", bintohex(blob, size));
	printf_vendor(&lhdc->info);
	printf(""
			"  <reserved>:2\n"
			"  bit-depth:2 =%s%s\n"
			"  frequency:2 =%s%s%s%s\n"
			"  low-latency:1 = %s\n"
			"  max-bit-rate:3 = %d\n"
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
			lhdc_get_max_bit_rate(lhdc),
			lhdc->version,
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_TWS_PLUS ? " TWS+" : "",
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_TWS ? " TWS" : "",
			lhdc->ch_split_mode & LHDC_CH_SPLIT_MODE_NONE ? " None" : "");
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
			"  frequency:2 =%s%s%s%s\n"
			"}\n",
			lhdc->ch_separation ? "true" : "false",
			lhdc->bit_depth & LHDC_BIT_DEPTH_24 ? " 24" : "",
			lhdc->bit_depth & LHDC_BIT_DEPTH_16 ? " 16" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_96000 ? " 96000" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_88200 ? " 88200" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_48000 ? " 48000" : "",
			lhdc->frequency & LHDC_SAMPLING_FREQ_44100 ? " 44100" : "");
}

static struct {
	uint16_t codec_id;
	size_t blob_size;
	void (*dump)(const void *, size_t);
} dumps[] = {
	{ A2DP_CODEC_SBC, sizeof(a2dp_sbc_t), dump_sbc },
	{ A2DP_CODEC_MPEG12, sizeof(a2dp_mpeg_t), dump_mpeg },
	{ A2DP_CODEC_MPEG24, sizeof(a2dp_aac_t), dump_aac },
	{ A2DP_CODEC_ATRAC, sizeof(a2dp_atrac_t), dump_atrac },
	{ A2DP_CODEC_VENDOR_APTX, sizeof(a2dp_aptx_t), dump_aptx },
	{ A2DP_CODEC_VENDOR_APTX_TWS, sizeof(a2dp_aptx_t), dump_aptx_tws },
	{ A2DP_CODEC_VENDOR_APTX_AD, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_APTX_HD, sizeof(a2dp_aptx_hd_t), dump_aptx_hd },
	{ A2DP_CODEC_VENDOR_APTX_LL, sizeof(a2dp_aptx_ll_t), dump_aptx_ll },
	{ A2DP_CODEC_VENDOR_APTX_LL, sizeof(a2dp_aptx_ll_new_t), dump_aptx_ll },
	{ A2DP_CODEC_VENDOR_FASTSTREAM, sizeof(a2dp_faststream_t), dump_faststream },
	{ A2DP_CODEC_VENDOR_LDAC, sizeof(a2dp_ldac_t), dump_ldac },
	{ A2DP_CODEC_VENDOR_LHDC, sizeof(a2dp_lhdc_t), dump_lhdc },
	{ A2DP_CODEC_VENDOR_LHDC_LL, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_LHDC_V1, sizeof(a2dp_lhdc_v1_t), dump_lhdc_v1 },
	{ A2DP_CODEC_VENDOR_SAMSUNG_HD, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_SAMSUNG_SC, -1, dump_vendor },
};

int dump(const char *config, bool detect) {

	uint16_t codec_id = get_codec(config);

	ssize_t blob_size;
	if ((blob_size = get_codec_blob(config, NULL, 0)) == -1)
		return -1;

	void *blob = malloc(blob_size);
	if (get_codec_blob(config, blob, blob_size) == -1)
		return -1;

	for (size_t i = 0; i < ARRAYSIZE(dumps); i++)
		if (dumps[i].codec_id == codec_id) {
			dumps[i].dump(blob, blob_size);
			return 0;
		}

	if (detect) {
		for (size_t i = 0; i < ARRAYSIZE(dumps); i++)
			if (dumps[i].blob_size == (size_t)blob_size)
				dumps[i].dump(blob, blob_size);
		dump_vendor(blob, blob_size);
		return 0;
	}

	fprintf(stderr, "Couldn't detect codec type: %s\n", config);
	return -1;
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
