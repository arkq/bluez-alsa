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

static const int hextable[255] = {
	['0'] = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
	['A'] = 10, 11, 12, 13, 14, 15,
	['a'] = 10, 11, 12, 13, 14, 15,
};

static const struct {
	uint16_t codec_id;
	const char *name;
} codecs[] = {
	{ A2DP_CODEC_SBC, "SBC" },
	{ A2DP_CODEC_MPEG12, "MP3" },
	{ A2DP_CODEC_MPEG12, "MPEG" },
	{ A2DP_CODEC_MPEG12, "MPEG12" },
	{ A2DP_CODEC_MPEG24, "AAC" },
	{ A2DP_CODEC_MPEG24, "MPEG24" },
	{ A2DP_CODEC_ATRAC, "ATRAC" },
	{ A2DP_CODEC_VENDOR_APTX, "aptX" },
	{ A2DP_CODEC_VENDOR_APTX_AD, "aptX-AD" },
	{ A2DP_CODEC_VENDOR_APTX_HD, "aptX-HD" },
	{ A2DP_CODEC_VENDOR_APTX_LL, "aptX-LL" },
	{ A2DP_CODEC_VENDOR_APTX_TWS, "aptX-TWS" },
	{ A2DP_CODEC_VENDOR_FASTSTREAM, "FastStream" },
	{ A2DP_CODEC_VENDOR_LDAC, "LDAC" },
	{ A2DP_CODEC_VENDOR_LHDC, "LHDC" },
	{ A2DP_CODEC_VENDOR_LHDC_V1, "LHDCv1" },
	{ A2DP_CODEC_VENDOR_LLAC, "LLAC" },
	{ A2DP_CODEC_VENDOR_SAMSUNG_HD, "samsung-HD" },
	{ A2DP_CODEC_VENDOR_SAMSUNG_SC, "samsung-SC" },
};

static uint16_t get_codec(const char *s) {

	const char *tmp;
	size_t len = strlen(s);
	if ((tmp = strchr(s, ':')) != NULL)
		len = tmp - s;

	if (len == 0)
		goto fail;

	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (strncasecmp(s, codecs[i].name, len) == 0)
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

	len /= 2;
	for (size_t i = 0; i < len; i++) {
		((char *)dest)[i] = hextable[(int)s[i * 2]] << 4;
		((char *)dest)[i] |= hextable[(int)s[i * 2 + 1]];
	}

	return len;
}

static char *bintohex(const void *src, size_t n) {
	char *hex = calloc(1, n * 2 + 1);
	for (size_t i = 0; i < n; i++)
		sprintf(&hex[i * 2], "%.2x", ((unsigned char *)src)[i]);
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

static void dump_vendor(const void *blob, size_t size) {
	const a2dp_vendor_codec_t *info = blob;
	if (size <= sizeof(*info))
		return;
	const void *data = info + 1;
	size_t data_size = size - sizeof(*info);
	printf("<hex:%s> {\n"
			"  vendor-id:32 = %#x [%s]\n"
			"  vendor-codec-id:16 = %#x\n"
			"  data:%zu = hex:%s\n"
			"}\n",
			bintohex(blob, size),
			A2DP_GET_VENDOR_ID(*info),
			bt_compidtostr(A2DP_GET_VENDOR_ID(*info)),
			A2DP_GET_CODEC_ID(*info),
			data_size * 8,
			bintohex(data, data_size));
}

static void _dump_aptx(const void *blob, size_t size, const char *name) {
	const a2dp_aptx_t *aptx = blob;
	if (check_blob_size(sizeof(*aptx), size) == -1)
		return;
	printf("%s <hex:%s> {\n"
			"  vendor-id:32 = %#x [%s]\n"
			"  vendor-codec-id:16 = %#x\n"
			"  channel-mode:4 =%s%s%s\n"
			"  sampling-frequency:4 =%s%s%s%s\n"
			"}\n",
			name,
			bintohex(aptx, sizeof(*aptx)),
			A2DP_GET_VENDOR_ID(aptx->info),
			bt_compidtostr(A2DP_GET_VENDOR_ID(aptx->info)),
			A2DP_GET_CODEC_ID(aptx->info),
			aptx->channel_mode & APTX_CHANNEL_MODE_STEREO ? " Stereo" : "",
			aptx->channel_mode & APTX_CHANNEL_MODE_TWS ? " DualChannel" : "",
			aptx->channel_mode & APTX_CHANNEL_MODE_MONO ? " Mono" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_48000 ? " 48000" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_44100 ? " 44100" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_32000 ? " 32000" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_16000 ? " 16000" : "");
}

static void dump_aptx(const void *blob, size_t size) {
	_dump_aptx(blob, size, "aptX");
}

static void dump_aptx_tws(const void *blob, size_t size) {
	_dump_aptx(blob, size, "aptX-TWS");
}

static void dump_aptx_hd(const void *blob, size_t size) {
	const a2dp_aptx_hd_t *aptx_hd = blob;
	if (check_blob_size(sizeof(*aptx_hd), size) == -1)
		return;
	printf("aptX HD <hex:%s> {\n"
			"  vendor-id:32 = %#x [%s]\n"
			"  vendor-codec-id:16 = %#x\n"
			"  channel-mode:4 =%s%s\n"
			"  sampling-frequency:4 =%s%s%s%s\n"
			"  <reserved>:32\n"
			"}\n",
			bintohex(aptx_hd, sizeof(*aptx_hd)),
			A2DP_GET_VENDOR_ID(aptx_hd->aptx.info),
			bt_compidtostr(A2DP_GET_VENDOR_ID(aptx_hd->aptx.info)),
			A2DP_GET_CODEC_ID(aptx_hd->aptx.info),
			aptx_hd->aptx.channel_mode & APTX_CHANNEL_MODE_STEREO ? " Stereo" : "",
			aptx_hd->aptx.channel_mode & APTX_CHANNEL_MODE_MONO ? " Mono" : "",
			aptx_hd->aptx.frequency & APTX_SAMPLING_FREQ_48000 ? " 48000" : "",
			aptx_hd->aptx.frequency & APTX_SAMPLING_FREQ_44100 ? " 44100" : "",
			aptx_hd->aptx.frequency & APTX_SAMPLING_FREQ_32000 ? " 32000" : "",
			aptx_hd->aptx.frequency & APTX_SAMPLING_FREQ_16000 ? " 16000" : "");
}

static void dump_faststream(const void *blob, size_t size) {
	const a2dp_faststream_t *faststream = blob;
	if (check_blob_size(sizeof(*faststream), size) == -1)
		return;
	printf("FastStream <hex:%s> {\n"
			"  vendor-id:32 = %#x [%s]\n"
			"  vendor-codec-id:16 = %#x\n"
			"  direction:8 =%s%s\n"
			"  sampling-frequency-voice:8 =%s\n"
			"  sampling-frequency-music:8 =%s%s\n"
			"}\n",
			bintohex(faststream, sizeof(*faststream)),
			A2DP_GET_VENDOR_ID(faststream->info),
			bt_compidtostr(A2DP_GET_VENDOR_ID(faststream->info)),
			A2DP_GET_CODEC_ID(faststream->info),
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
	printf("LDAC <hex:%s> {\n"
			"  vendor-id:32 = %#x [%s]\n"
			"  vendor-codec-id:16 = %#x\n"
			"  <reserved>:2\n"
			"  sampling-frequency:6 =%s%s%s%s%s%s\n"
			"  <reserved>:5\n"
			"  channel-mode:3 =%s%s%s\n"
			"}\n",
			bintohex(ldac, sizeof(*ldac)),
			A2DP_GET_VENDOR_ID(ldac->info),
			bt_compidtostr(A2DP_GET_VENDOR_ID(ldac->info)),
			A2DP_GET_CODEC_ID(ldac->info),
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
	{ A2DP_CODEC_VENDOR_APTX_LL, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_FASTSTREAM, sizeof(a2dp_faststream_t), dump_faststream },
	{ A2DP_CODEC_VENDOR_LDAC, sizeof(a2dp_ldac_t), dump_ldac },
	{ A2DP_CODEC_VENDOR_LHDC, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_LHDC_V1, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_LLAC, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_SAMSUNG_HD, -1, dump_vendor },
	{ A2DP_CODEC_VENDOR_SAMSUNG_SC, -1, dump_vendor },
};

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVx";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "auto-detect", no_argument, NULL, 'x' },
		{ 0, 0, 0, 0 },
	};

	bool detect = false;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <CODEC>\n"
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

	if (argc - optind != 1)
		goto usage;

	const char *codec = argv[optind];
	uint16_t codec_id = get_codec(codec);

	ssize_t blob_size;
	if ((blob_size = get_codec_blob(codec, NULL, 0)) == -1)
		return EXIT_FAILURE;

	void *blob = malloc(blob_size);
	if (get_codec_blob(codec, blob, blob_size) == -1)
		return EXIT_FAILURE;

	for (size_t i = 0; i < ARRAYSIZE(dumps); i++)
		if (dumps[i].codec_id == codec_id) {
			dumps[i].dump(blob, blob_size);
			return EXIT_SUCCESS;
		}

	if (detect) {
		for (size_t i = 0; i < ARRAYSIZE(dumps); i++)
			if (dumps[i].blob_size == (size_t)blob_size)
				dumps[i].dump(blob, blob_size);
		dump_vendor(blob, blob_size);
	}
	else {
		fprintf(stderr, "Couldn't detect codec type: %s\n", codec);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
