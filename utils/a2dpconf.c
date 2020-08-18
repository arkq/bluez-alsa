/*
 * BlueALSA - a2dpconf.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
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

	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (strncasecmp(s, codecs[i].name, len) == 0)
			return codecs[i].codec_id;

	return 0xFFFF;
}

static int get_codec_blob(const char *s, void *dest, size_t n) {

	const char *tmp;
	if ((tmp = strchr(s, ':')) == NULL)
		return -1;

	s = tmp + 1;

	size_t len;
	if ((len = strlen(s)) != n * 2) {
		fprintf(stderr, "Invalid codec blob size: %zd != %zd\n", len, n * 2);
		return -1;
	}

	for (size_t i = 0; i < n; i++) {
		((char *)dest)[i] = hextable[(int)s[i * 2]] << 4;
		((char *)dest)[i] |= hextable[(int)s[i * 2 + 1]];
	}

	return 0;
}

static char *bintohex(const void *src, size_t n) {
	char *hex = calloc(1, n * 2 + 1);
	for (size_t i = 0; i < n; i++)
		sprintf(&hex[i * 2], "%.2x", ((unsigned char *)src)[i]);
	return hex;
}

static void dump_sbc(const a2dp_sbc_t *sbc) {
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

static void dump_mpeg(const a2dp_mpeg_t *mpeg) {
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

static void dump_aac(const a2dp_aac_t *aac) {
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

static void dump_atrac(const a2dp_atrac_t *atrac) {
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

static void dump_aptx(const a2dp_aptx_t *aptx) {
	printf("aptX <hex:%s> {\n"
			"  vendor-id:32 = %#x\n"
			"  vendor-codec-id:16 = %#x\n"
			"  channel-mode:4 =%s%s%s\n"
			"  sampling-frequency:4 =%s%s%s%s\n"
			"}\n",
			bintohex(aptx, sizeof(*aptx)),
			A2DP_GET_VENDOR_ID(aptx->info),
			A2DP_GET_CODEC_ID(aptx->info),
			aptx->channel_mode & APTX_CHANNEL_MODE_STEREO ? " Stereo" : "",
			aptx->channel_mode & APTX_CHANNEL_MODE_TWS ? " DualChannel" : "",
			aptx->channel_mode & APTX_CHANNEL_MODE_MONO ? " Mono" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_48000 ? " 48000" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_44100 ? " 44100" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_32000 ? " 32000" : "",
			aptx->frequency & APTX_SAMPLING_FREQ_16000 ? " 16000" : "");
}

static void dump_aptx_hd(const a2dp_aptx_hd_t *aptx_hd) {
	printf("aptX HD <hex:%s> {\n"
			"  vendor-id:32 = %#x\n"
			"  vendor-codec-id:16 = %#x\n"
			"  channel-mode:4 =%s%s\n"
			"  sampling-frequency:4 =%s%s%s%s\n"
			"  <reserved>:32\n"
			"}\n",
			bintohex(aptx_hd, sizeof(*aptx_hd)),
			A2DP_GET_VENDOR_ID(aptx_hd->aptx.info),
			A2DP_GET_CODEC_ID(aptx_hd->aptx.info),
			aptx_hd->aptx.channel_mode & APTX_CHANNEL_MODE_STEREO ? " Stereo" : "",
			aptx_hd->aptx.channel_mode & APTX_CHANNEL_MODE_MONO ? " Mono" : "",
			aptx_hd->aptx.frequency & APTX_SAMPLING_FREQ_48000 ? " 48000" : "",
			aptx_hd->aptx.frequency & APTX_SAMPLING_FREQ_44100 ? " 44100" : "",
			aptx_hd->aptx.frequency & APTX_SAMPLING_FREQ_32000 ? " 32000" : "",
			aptx_hd->aptx.frequency & APTX_SAMPLING_FREQ_16000 ? " 16000" : "");
}

static void dump_faststream(const a2dp_faststream_t *faststream) {
	printf("FastStream <hex:%s> {\n"
			"  vendor-id:32 = %#x\n"
			"  vendor-codec-id:16 = %#x\n"
			"  direction:8 =%s%s\n"
			"  sampling-frequency-voice:8 =%s\n"
			"  sampling-frequency-music:8 =%s%s\n"
			"}\n",
			bintohex(faststream, sizeof(*faststream)),
			A2DP_GET_VENDOR_ID(faststream->info),
			A2DP_GET_CODEC_ID(faststream->info),
			faststream->direction & FASTSTREAM_DIRECTION_MUSIC ? " Music" : "",
			faststream->direction & FASTSTREAM_DIRECTION_VOICE ? " Voice" : "",
			faststream->frequency_voice & FASTSTREAM_SAMPLING_FREQ_VOICE_16000 ? " 16000" : "",
			faststream->frequency_music & FASTSTREAM_SAMPLING_FREQ_MUSIC_48000 ? " 48000" : "",
			faststream->frequency_music & FASTSTREAM_SAMPLING_FREQ_MUSIC_44100 ? " 44100" : "");
}

static void dump_ldac(const a2dp_ldac_t *ldac) {
	printf("LDAC <hex:%s> {\n"
			"  vendor-id:32 = %#x\n"
			"  vendor-codec-id:16 = %#x\n"
			"  <reserved>:2\n"
			"  sampling-frequency:6 =%s%s%s%s%s%s\n"
			"  <reserved>:5\n"
			"  channel-mode:3 =%s%s%s\n"
			"}\n",
			bintohex(ldac, sizeof(*ldac)),
			A2DP_GET_VENDOR_ID(ldac->info),
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

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hV";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <CODEC>\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"\nExamples:\n"
					"  %s sbc:ffff0235\n"
					"  %s aptx:4f0000000100ff\n",
					argv[0], argv[0], argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (argc - optind != 1)
		goto usage;

	const char *codec = argv[optind];
	switch (get_codec(codec)) {

	case A2DP_CODEC_SBC: {
		a2dp_sbc_t sbc = { 0 };
		if (get_codec_blob(codec, &sbc, sizeof(sbc)) != -1)
			dump_sbc(&sbc);
	} break;

	case A2DP_CODEC_MPEG12: {
		a2dp_mpeg_t mpeg = { 0 };
		if (get_codec_blob(codec, &mpeg, sizeof(mpeg)) != -1)
			dump_mpeg(&mpeg);
	} break;

	case A2DP_CODEC_MPEG24: {
		a2dp_aac_t aac = { 0 };
		if (get_codec_blob(codec, &aac, sizeof(aac)) != -1)
			dump_aac(&aac);
	} break;

	case A2DP_CODEC_ATRAC: {
		a2dp_atrac_t atrac = { 0 };
		if (get_codec_blob(codec, &atrac, sizeof(atrac)) != -1)
			dump_atrac(&atrac);
	} break;

	case A2DP_CODEC_VENDOR_APTX:
	case A2DP_CODEC_VENDOR_APTX_TWS: {
		a2dp_aptx_t aptx = { 0 };
		if (get_codec_blob(codec, &aptx, sizeof(aptx)) != -1)
			dump_aptx(&aptx);
	} break;

	case A2DP_CODEC_VENDOR_APTX_AD: {
	} break;

	case A2DP_CODEC_VENDOR_APTX_HD: {
		a2dp_aptx_hd_t aptx_hd = { 0 };
		if (get_codec_blob(codec, &aptx_hd, sizeof(aptx_hd)) != -1)
			dump_aptx_hd(&aptx_hd);
	} break;

	case A2DP_CODEC_VENDOR_APTX_LL: {
	} break;

	case A2DP_CODEC_VENDOR_FASTSTREAM: {
		a2dp_faststream_t faststream = { 0 };
		if (get_codec_blob(codec, &faststream, sizeof(faststream)) != -1)
			dump_faststream(&faststream);
	} break;

	case A2DP_CODEC_VENDOR_LDAC: {
		a2dp_ldac_t ldac = { 0 };
		if (get_codec_blob(codec, &ldac, sizeof(ldac)) != -1)
			dump_ldac(&ldac);
	} break;

	case A2DP_CODEC_VENDOR_LHDC: {
	} break;

	case A2DP_CODEC_VENDOR_LHDC_V1: {
	} break;

	case A2DP_CODEC_VENDOR_LLAC: {
	} break;

	case A2DP_CODEC_VENDOR_SAMSUNG_HD: {
	} break;

	case A2DP_CODEC_VENDOR_SAMSUNG_SC: {
	} break;

	default:
		fprintf(stderr, "Couldn't detect codec type: %s\n", codec);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
