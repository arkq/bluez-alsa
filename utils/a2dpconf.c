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
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>

#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/hex.h"

static uint32_t get_codec(const char *s) {

	char buffer[32] = { 0 };
	char *tmp;

	strncpy(buffer, s, sizeof(buffer) - 1);
	if ((tmp = strchr(buffer, ':')) != NULL)
		tmp[0] = '\0';

	if (strcasecmp(buffer, "vendor") == 0)
		return A2DP_CODEC_VENDOR;

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

static void print_bits(const void *bstream, size_t offset, size_t n, size_t size) {

	char mask[] = ".... .... .... .... .... .... .... ....";
	size_t mask_spaces = size == 0 ? 0 : (size - 1) / 4;
	mask[size + mask_spaces] = '\0';

	for (size_t i = offset; i < offset + n; i++) {
		size_t spaces = i == 0 ? 0 : i / 4;
		uint8_t byte = ((uint8_t *)bstream)[i / 8];
		uint8_t bit = 1 << (8 - (i % 8) - 1);
		mask[i + spaces] = byte & bit ? '1' : '0';
	}

	printf("  %s", mask);

}

struct bitfield {
	uint32_t value;
	const char *label;
};

static bool verbose = false;

static void print_bitfield(const char *name, const void *bstream,
		size_t offset, size_t n, size_t size, uint32_t value,
		const struct bitfield *fields) {
	if (!verbose) {
		printf("  %s (", name);
		int elems = 0;
		if (fields == NULL)
			printf(" %s", value ? "true" : "false");
		else
			for (const struct bitfield *f = fields; f->label != NULL; f++)
				if (f->value == 0 || value & f->value)
					printf("%s%s", elems++ == 0 ? " " : " | ", f->label);
		printf(" )\n");
	}
	else {
		for (size_t i = 0; i < n; i++) {
			uint32_t bit = 1 << (n - i - 1);
			print_bits(bstream, offset + i, 1, size);
			printf(" = %s", name);
			if (fields != NULL)
				for (const struct bitfield *f = fields; f->label != NULL; f++)
					if (bit & f->value)
						printf(" %s", f->label);
			printf(": %s\n", bit & value ? "true" : "false");
		}
	}
}

static void print_value(const char *name, const void *bstream,
		size_t offset, size_t n, size_t size, const char *format, ...) {

	va_list ap;
	va_start(ap, format);

	if (!verbose) {
		printf("  %s ( ", name);
		vprintf(format, ap);
		printf(" )\n");
	}
	else {
		print_bits(bstream, offset, n, size);
		printf(" = %s: ", name);
		vprintf(format, ap);
		printf("\n");
	}

	va_end(ap);

}

#define print_bitfield8(name, bstream, offset, n, value, fields) \
	print_bitfield(name, bstream, offset, n, 8, value, fields)
#define print_bitfield16(name, bstream, offset, n, value, fields) \
	print_bitfield(name, bstream, offset, n, 16, value, fields)
#define print_bitfield24(name, bstream, offset, n, value, fields) \
	print_bitfield(name, bstream, offset, n, 24, value, fields)
#define print_bitfield32(name, bstream, offset, n, value, fields) \
	print_bitfield(name, bstream, offset, n, 32, value, fields)

#define print_bool8(name, bstream, offset, n, value) \
	print_bitfield(name, bstream, offset, n, 8, value, NULL)
#define print_bool16(name, bstream, offset, n, value) \
	print_bitfield(name, bstream, offset, n, 16, value, NULL)
#define print_bool24(name, bstream, offset, n, value) \
	print_bitfield(name, bstream, offset, n, 24, value, NULL)
#define print_bool32(name, bstream, offset, n, value) \
	print_bitfield(name, bstream, offset, n, 32, value, NULL)

#define print_value8(name, bstream, offset, n, format, ...) \
	print_value(name, bstream, offset, n, 8, format, __VA_ARGS__)
#define print_value16(name, bstream, offset, n, format, ...) \
	print_value(name, bstream, offset, n, 16, format, __VA_ARGS__)
#define print_value24(name, bstream, offset, n, format, ...) \
	print_value(name, bstream, offset, n, 24, format, __VA_ARGS__)
#define print_value32(name, bstream, offset, n, format, ...) \
	print_value(name, bstream, offset, n, 32, format, __VA_ARGS__)

static void dump_sbc(const void *blob, size_t size) {

	const a2dp_sbc_t *sbc = blob;
	if (check_blob_size(sizeof(*sbc), size) == -1)
		return;

	static const struct bitfield ch_modes[] = {
		{ SBC_CHANNEL_MODE_MONO, "Mono" },
		{ SBC_CHANNEL_MODE_DUAL_CHANNEL, "Dual Channel" },
		{ SBC_CHANNEL_MODE_STEREO, "Stereo" },
		{ SBC_CHANNEL_MODE_JOINT_STEREO, "Joint Stereo" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ SBC_SAMPLING_FREQ_16000, "16000 Hz" },
		{ SBC_SAMPLING_FREQ_32000, "32000 Hz" },
		{ SBC_SAMPLING_FREQ_44100, "44100 Hz" },
		{ SBC_SAMPLING_FREQ_48000, "48000 Hz" },
		{ 0 },
	};

	static const struct bitfield blocks[] = {
		{ SBC_BLOCK_LENGTH_4, "4" },
		{ SBC_BLOCK_LENGTH_8, "8" },
		{ SBC_BLOCK_LENGTH_12, "12" },
		{ SBC_BLOCK_LENGTH_16, "16" },
		{ 0 },
	};

	static const struct bitfield bands[] = {
		{ SBC_SUBBANDS_4, "4" },
		{ SBC_SUBBANDS_8, "8" },
		{ 0 },
	};

	static const struct bitfield allocs[] = {
		{ SBC_ALLOCATION_SNR, "SNR" },
		{ SBC_ALLOCATION_LOUDNESS, "Loudness" },
		{ 0 },
	};

	const uint8_t *bstream = blob;
	printf("SBC <hex:%s> {\n", bintohex(sbc, sizeof(*sbc)));
	print_bitfield8("Sample Rate", bstream, 0, 4, sbc->sampling_freq, rates);
	print_bitfield8("Channel Mode", bstream, 4, 4, sbc->channel_mode, ch_modes);
	print_bitfield8("Block Length", bstream += 1, 0, 4, sbc->block_length, blocks);
	print_bitfield8("Sub-bands", bstream, 4, 2, sbc->subbands, bands);
	print_bitfield8("Allocation Method", bstream, 6, 2, sbc->allocation_method, allocs);
	print_value8("Min Bit-pool", bstream += 1, 0, 8, "%u", sbc->min_bitpool);
	print_value8("Max Bit-pool", bstream += 1, 0, 8, "%u", sbc->max_bitpool);
	printf("}\n");

}

static void dump_mpeg(const void *blob, size_t size) {

	const a2dp_mpeg_t *mpeg = blob;
	if (check_blob_size(sizeof(*mpeg), size) == -1)
		return;

	static const struct bitfield layers[] = {
		{ MPEG_LAYER_MP1, "MP1" },
		{ MPEG_LAYER_MP2, "MP2" },
		{ MPEG_LAYER_MP3, "MP3" },
		{ 0 },
	};

	static const struct bitfield ch_modes[] = {
		{ MPEG_CHANNEL_MODE_MONO, "Mono" },
		{ MPEG_CHANNEL_MODE_DUAL_CHANNEL, "Dual Channel" },
		{ MPEG_CHANNEL_MODE_STEREO, "Stereo" },
		{ MPEG_CHANNEL_MODE_JOINT_STEREO, "Joint Stereo" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ MPEG_SAMPLING_FREQ_16000, "16000 Hz" },
		{ MPEG_SAMPLING_FREQ_22050, "22050 Hz" },
		{ MPEG_SAMPLING_FREQ_24000, "24000 Hz" },
		{ MPEG_SAMPLING_FREQ_32000, "32000 Hz" },
		{ MPEG_SAMPLING_FREQ_44100, "44100 Hz" },
		{ MPEG_SAMPLING_FREQ_48000, "48000 Hz" },
		{ 0 },
	};

	static const struct bitfield mpfs[] = {
		{ 0, "MPF-1" },
		{ 1, "MPF-2" },
		{ 0 },
	};

	static const struct bitfield indexes[] = {
		{ MPEG_BITRATE_INDEX_0, "0" },
		{ MPEG_BITRATE_INDEX_1, "1" },
		{ MPEG_BITRATE_INDEX_2, "2" },
		{ MPEG_BITRATE_INDEX_3, "3" },
		{ MPEG_BITRATE_INDEX_4, "4" },
		{ MPEG_BITRATE_INDEX_5, "5" },
		{ MPEG_BITRATE_INDEX_6, "6" },
		{ MPEG_BITRATE_INDEX_7, "7" },
		{ MPEG_BITRATE_INDEX_8, "8" },
		{ MPEG_BITRATE_INDEX_9, "9" },
		{ MPEG_BITRATE_INDEX_10, "10" },
		{ MPEG_BITRATE_INDEX_11, "11" },
		{ MPEG_BITRATE_INDEX_12, "12" },
		{ MPEG_BITRATE_INDEX_13, "13" },
		{ MPEG_BITRATE_INDEX_14, "14" },
		{ 0 },
	};

	static const struct bitfield indexes1[] = {
		{ MPEG_BITRATE_INDEX_8 >> 8, "8" },
		{ MPEG_BITRATE_INDEX_9 >> 8, "9" },
		{ MPEG_BITRATE_INDEX_10 >> 8, "10" },
		{ MPEG_BITRATE_INDEX_11 >> 8, "11" },
		{ MPEG_BITRATE_INDEX_12 >> 8, "12" },
		{ MPEG_BITRATE_INDEX_13 >> 8, "13" },
		{ MPEG_BITRATE_INDEX_14 >> 8, "14" },
		{ 0 },
	};

	const uint8_t *bstream = blob;
	printf("MPEG-1,2 Audio <hex:%s> {\n", bintohex(mpeg, sizeof(*mpeg)));
	print_bitfield8("Layer", bstream, 0, 3, mpeg->layer, layers);
	print_bool8("CRC", bstream, 3, 1, mpeg->crc);
	print_bitfield8("Channel Mode", bstream, 4, 4, mpeg->channel_mode, ch_modes);
	print_value8("RFA", bstream += 1, 0, 1, "%u", mpeg->rfa);
	print_bitfield8("Media Payload Format", bstream, 1, 1, mpeg->mpf, mpfs);
	print_bitfield8("Sample Rate", bstream, 2, 6, mpeg->sampling_freq, rates);
	print_bool8("VBR", bstream += 1, 0, 1, mpeg->vbr);
	if (!verbose) {
		const uint16_t mpeg_bitrate = A2DP_MPEG_GET_BITRATE(*mpeg);
		print_bitfield16("Bitrate Index", bstream, 1, 15, mpeg_bitrate, indexes);
	}
	else {
		print_bitfield8("Bitrate Index", bstream, 1, 7, mpeg->bitrate1, indexes1);
		print_bitfield8("Bitrate Index", bstream += 1, 0, 8, mpeg->bitrate2, indexes);
	}
	printf("}\n");

}

static void dump_aac(const void *blob, size_t size) {

	const a2dp_aac_t *aac = blob;
	if (check_blob_size(sizeof(*aac), size) == -1)
		return;

	static const struct bitfield objects[] = {
		{ AAC_OBJECT_TYPE_MPEG2_LC, "MPEG2-LC" },
		{ AAC_OBJECT_TYPE_MPEG4_LC, "MPEG4-LC" },
		{ AAC_OBJECT_TYPE_MPEG4_LTP, "MPEG4-LTP" },
		{ AAC_OBJECT_TYPE_MPEG4_SCA, "MPEG4-SCA" },
		{ AAC_OBJECT_TYPE_MPEG4_HE, "MPEG4-HE" },
		{ AAC_OBJECT_TYPE_MPEG4_HE2, "MPEG4-HE2" },
		{ AAC_OBJECT_TYPE_MPEG4_ELD2, "MPEG4-ELD2" },
		{ 0 },
	};

	static const struct bitfield ch_modes[] = {
		{ AAC_CHANNEL_MODE_MONO, "Mono" },
		{ AAC_CHANNEL_MODE_STEREO, "Stereo" },
		{ AAC_CHANNEL_MODE_5_1, "Surround-5.1" },
		{ AAC_CHANNEL_MODE_7_1, "Surround-7.1" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ AAC_SAMPLING_FREQ_8000, "8000 Hz" },
		{ AAC_SAMPLING_FREQ_11025, "11025 Hz" },
		{ AAC_SAMPLING_FREQ_12000, "12000 Hz" },
		{ AAC_SAMPLING_FREQ_16000, "16000 Hz" },
		{ AAC_SAMPLING_FREQ_22050, "22050 Hz" },
		{ AAC_SAMPLING_FREQ_24000, "24000 Hz" },
		{ AAC_SAMPLING_FREQ_32000, "32000 Hz" },
		{ AAC_SAMPLING_FREQ_44100, "44100 Hz" },
		{ AAC_SAMPLING_FREQ_48000, "48000 Hz" },
		{ AAC_SAMPLING_FREQ_64000, "64000 Hz" },
		{ AAC_SAMPLING_FREQ_88200, "88200 Hz" },
		{ AAC_SAMPLING_FREQ_96000, "96000 Hz" },
		{ 0 },
	};

	static const struct bitfield rates1[] = {
		{ AAC_SAMPLING_FREQ_8000 >> 4, "8000 Hz" },
		{ AAC_SAMPLING_FREQ_11025 >> 4, "11025 Hz" },
		{ AAC_SAMPLING_FREQ_12000 >> 4, "12000 Hz" },
		{ AAC_SAMPLING_FREQ_16000 >> 4, "16000 Hz" },
		{ AAC_SAMPLING_FREQ_22050 >> 4, "22050 Hz" },
		{ AAC_SAMPLING_FREQ_24000 >> 4, "24000 Hz" },
		{ AAC_SAMPLING_FREQ_32000 >> 4, "32000 Hz" },
		{ AAC_SAMPLING_FREQ_44100 >> 4, "44100 Hz" },
		{ 0 },
	};

	const uint8_t *bstream = blob;
	printf("MPEG-2,4 AAC <hex:%s> {\n", bintohex(aac, sizeof(*aac)));
	print_bitfield8("Object Type", bstream, 0, 7, aac->object_type, objects);
	print_bool8("Dynamic Range Control", bstream, 7, 1, aac->drc);
	if (!verbose) {
		const uint16_t aac_sampling_freq = A2DP_AAC_GET_SAMPLING_FREQ(*aac);
		print_bitfield16("Sample Rate", bstream += 1, 0, 12, aac_sampling_freq, rates);
		bstream += 1;
	}
	else {
		print_bitfield8("Sample Rate", bstream += 1, 0, 8, aac->sampling_freq1, rates1);
		print_bitfield8("Sample Rate", bstream += 1, 0, 4, aac->sampling_freq2, rates);
	}
	print_bitfield8("Channel Mode", bstream, 4, 4, aac->channel_mode, ch_modes);
	print_bool24("VBR", bstream += 1, 0, 1, aac->vbr);
	print_value24("Bitrate", bstream, 1, 23, "%u", A2DP_AAC_GET_BITRATE(*aac));
	printf("}\n");

}

static void dump_usac(const void *blob, size_t size) {

	const a2dp_usac_t *usac = blob;
	if (check_blob_size(sizeof(*usac), size) == -1)
		return;

	static const struct bitfield objects[] = {
		{ USAC_OBJECT_TYPE_MPEGD_DRC, "MPEG-D-DRC" },
		{ 1 << 0, "RFA" },
		{ 0 },
	};

	static const struct bitfield ch_modes[] = {
		{ USAC_CHANNEL_MODE_MONO, "Mono" },
		{ USAC_CHANNEL_MODE_STEREO, "Stereo" },
		{ 1 << 1, "RFA" },
		{ 1 << 0, "RFA" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ USAC_SAMPLING_FREQ_7350, "7350 Hz" },
		{ USAC_SAMPLING_FREQ_8000, "8000 Hz" },
		{ USAC_SAMPLING_FREQ_8820, "8820 Hz" },
		{ USAC_SAMPLING_FREQ_9600, "9600 Hz" },
		{ USAC_SAMPLING_FREQ_11025, "11025 Hz" },
		{ USAC_SAMPLING_FREQ_11760, "11760 Hz" },
		{ USAC_SAMPLING_FREQ_12000, "12000 Hz" },
		{ USAC_SAMPLING_FREQ_12800, "12800 Hz" },
		{ USAC_SAMPLING_FREQ_14700, "14700 Hz" },
		{ USAC_SAMPLING_FREQ_16000, "16000 Hz" },
		{ USAC_SAMPLING_FREQ_17640, "17640 Hz" },
		{ USAC_SAMPLING_FREQ_19200, "19200 Hz" },
		{ USAC_SAMPLING_FREQ_22050, "22050 Hz" },
		{ USAC_SAMPLING_FREQ_24000, "24000 Hz" },
		{ USAC_SAMPLING_FREQ_29400, "29400 Hz" },
		{ USAC_SAMPLING_FREQ_32000, "32000 Hz" },
		{ USAC_SAMPLING_FREQ_35280, "35280 Hz" },
		{ USAC_SAMPLING_FREQ_38400, "38400 Hz" },
		{ USAC_SAMPLING_FREQ_44100, "44100 Hz" },
		{ USAC_SAMPLING_FREQ_48000, "48000 Hz" },
		{ USAC_SAMPLING_FREQ_58800, "58800 Hz" },
		{ USAC_SAMPLING_FREQ_64000, "64000 Hz" },
		{ USAC_SAMPLING_FREQ_70560, "70560 Hz" },
		{ USAC_SAMPLING_FREQ_76800, "76800 Hz" },
		{ USAC_SAMPLING_FREQ_88200, "88200 Hz" },
		{ USAC_SAMPLING_FREQ_96000, "96000 Hz" },
		{ 0 },
	};

	const uint8_t *bstream = blob;
	printf("MPEG-D USAC <hex:%s> {\n", bintohex(usac, sizeof(*usac)));
	print_bitfield32("Object Type", bstream, 0, 2, usac->object_type, objects);
	const uint32_t usac_sampling_freq = A2DP_USAC_GET_SAMPLING_FREQ(*usac);
	print_bitfield32("Sample Rate", bstream, 2, 26, usac_sampling_freq, rates);
	print_bitfield32("Channel Mode", bstream, 28, 4, usac->channel_mode, ch_modes);
	print_bool24("VBR", bstream += 4, 0, 1, usac->vbr);
	print_value24("Bitrate", bstream, 1, 23, "%u", A2DP_USAC_GET_BITRATE(*usac));
	printf("}\n");

}

static void dump_atrac(const void *blob, size_t size) {

	const a2dp_atrac_t *atrac = blob;
	if (check_blob_size(sizeof(*atrac), size) == -1)
		return;

	static const struct bitfield ch_modes[] = {
		{ ATRAC_CHANNEL_MODE_MONO, "Mono" },
		{ ATRAC_CHANNEL_MODE_DUAL_CHANNEL, "Dual Channel" },
		{ ATRAC_CHANNEL_MODE_JOINT_STEREO, "Joint Stereo" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ ATRAC_SAMPLING_FREQ_44100, "44100 Hz" },
		{ ATRAC_SAMPLING_FREQ_48000, "48000 Hz" },
		{ 0 },
	};

	static const struct bitfield indexes[] = {
		{ 1 << 18, "0" },
		{ 1 << 17, "1" },
		{ 1 << 16, "2" },
		{ 1 << 15, "3" },
		{ 1 << 14, "4" },
		{ 1 << 13, "5" },
		{ 1 << 12, "6" },
		{ 1 << 11, "7" },
		{ 1 << 10, "8" },
		{ 1 << 9, "9" },
		{ 1 << 8, "10" },
		{ 1 << 7, "11" },
		{ 1 << 6, "12" },
		{ 1 << 5, "13" },
		{ 1 << 4, "14" },
		{ 1 << 3, "15" },
		{ 1 << 2, "16" },
		{ 1 << 1, "17" },
		{ 1 << 0, "18" },
		{ 0 },
	};

	const uint8_t *bstream = blob;
	printf("ATRAC <hex:%s> {\n", bintohex(atrac, sizeof(*atrac)));
	print_value8("Version", bstream, 0, 3, "%u", atrac->version);
	print_bitfield8("Channel Mode", bstream, 3, 3, atrac->channel_mode, ch_modes);
	print_value8("RFA", bstream, 6, 2, "%#x", atrac->rfa1);
	print_value24("RFA", ++bstream, 0, 2, "%#x", atrac->rfa2);
	print_bitfield24("Sample Rate", bstream, 2, 2, atrac->sampling_freq, rates);
	print_bool24("VBR", bstream, 4, 1, atrac->vbr);
	const uint32_t atrac_bitrate = A2DP_ATRAC_GET_BITRATE(*atrac);
	print_bitfield24("Bitrate Index", bstream, 5, 19, atrac_bitrate, indexes);
	const uint16_t atrac_max_sul = A2DP_ATRAC_GET_MAX_SUL(*atrac);
	print_value16("Max Sound Unit Length", bstream += 3, 0, 16, "%u", atrac_max_sul);
	print_value8("RFA", bstream += 2, 0, 8, "%#x", atrac->rfa3);
	printf("}\n");

}

static void print_vendor(const a2dp_vendor_info_t *info) {
	const uint32_t vendor_id = A2DP_VENDOR_INFO_GET_VENDOR_ID(*info);
	const char *vendor_name = bt_compidtostr(vendor_id);
	print_value32("Vendor ID", &info->vendor_id, 0, 32, "%#010x [%s]", vendor_id, vendor_name);
	const uint16_t codec_id = A2DP_VENDOR_INFO_GET_CODEC_ID(*info);
	print_value16("Vendor Codec ID", &info->codec_id, 0, 16, "%#06x", codec_id);
}

static void dump_vendor(const void *blob, size_t size) {

	const a2dp_vendor_info_t *info = blob;
	if (size <= sizeof(*info))
		return;

	printf("<hex:%s> {\n", bintohex(blob, size));
	print_vendor(info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	printf("  Data ( hex:%s )\n", bintohex(bstream, size - sizeof(*info)));
	printf("}\n");

}

static const struct bitfield aptx_ch_modes[] = {
	{ APTX_CHANNEL_MODE_MONO, "Mono" },
	{ APTX_CHANNEL_MODE_STEREO, "Stereo" },
	{ APTX_CHANNEL_MODE_TWS, "TWS" },
	{ 0 },
};

static const struct bitfield aptx_rates[] = {
	{ APTX_SAMPLING_FREQ_16000, "16000 Hz" },
	{ APTX_SAMPLING_FREQ_32000, "32000 Hz" },
	{ APTX_SAMPLING_FREQ_44100, "44100 Hz" },
	{ APTX_SAMPLING_FREQ_48000, "48000 Hz" },
	{ 0 },
};

static void dump_aptx(const void *blob, size_t size) {

	const a2dp_aptx_t *aptx = blob;
	if (check_blob_size(sizeof(*aptx), size) == -1)
		return;

	printf("aptX <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&aptx->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_bitfield8("Sample Rate", bstream, 0, 4, aptx->sampling_freq, aptx_rates);
	print_bitfield8("Channel Mode", bstream, 4, 4, aptx->channel_mode, aptx_ch_modes);
	printf("}\n");

}

static void dump_aptx_tws(const void *blob, size_t size) {

	const a2dp_aptx_t *aptx = blob;
	if (check_blob_size(sizeof(*aptx), size) == -1)
		return;
	printf("aptX-TWS <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&aptx->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_bitfield8("Sample Rate", bstream, 0, 4, aptx->sampling_freq, aptx_rates);
	print_bitfield8("Channel Mode", bstream, 4, 4, aptx->channel_mode, aptx_ch_modes);
	printf("}\n");

}

static void dump_aptx_ad(const void *blob, size_t size) {

	const a2dp_aptx_ad_t *aptx = blob;
	if (check_blob_size(sizeof(*aptx), size) == -1)
		return;

	static const struct bitfield ch_modes[] = {
		{ APTX_AD_CHANNEL_MODE_MONO, "Mono" },
		{ APTX_AD_CHANNEL_MODE_STEREO, "Stereo" },
		{ APTX_AD_CHANNEL_MODE_TWS, "TWS" },
		{ APTX_AD_CHANNEL_MODE_JOINT_STEREO, "Joint Stereo" },
		{ APTX_AD_CHANNEL_MODE_TWS_MONO, "TWS-Mono" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ APTX_AD_SAMPLING_FREQ_44100, "44100 Hz" },
		{ APTX_AD_SAMPLING_FREQ_48000, "48000 Hz" },
		{ APTX_AD_SAMPLING_FREQ_88000, "88000 Hz" },
		{ APTX_AD_SAMPLING_FREQ_192000, "192000 Hz" },
		{ 0 },
	};

	printf("aptX Adaptive <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&aptx->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_bitfield8("Sample Rate", bstream, 0, 5, aptx->sampling_freq, rates);
	print_value8("RFA", bstream, 5, 3, "%#x", aptx->rfa1);
	print_value8("RFA", bstream += 1, 0, 3, "%#x", aptx->rfa2);
	print_bitfield8("Channel Mode", bstream, 3, 5, aptx->channel_mode, ch_modes);
	print_value8("TTP-LL Low", bstream += 1, 0, 8, "%u", aptx->ttp_ll_low);
	print_value8("TTP-LL High", bstream += 1, 0, 8, "%u", aptx->ttp_ll_high);
	print_value8("TTP-HQ Low", bstream += 1, 0, 8, "%u", aptx->ttp_hq_low);
	print_value8("TTP-HQ High", bstream += 1, 0, 8, "%u", aptx->ttp_hq_high);
	print_value8("TTP-TWS Low", bstream += 1, 0, 8, "%u", aptx->ttp_tws_low);
	print_value8("TTP-TWS High", bstream += 1, 0, 8, "%u", aptx->ttp_tws_high);
	print_value24("EOC", bstream += 1, 0, 24, "hex:%02x%02x%02x", aptx->eoc[0], aptx->eoc[1], aptx->eoc[2]);
	printf("}\n");

}

static void dump_aptx_hd(const void *blob, size_t size) {

	const a2dp_aptx_hd_t *aptx = blob;
	if (check_blob_size(sizeof(*aptx), size) == -1)
		return;

	printf("aptX HD <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&aptx->aptx.info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_bitfield8("Sample Rate", bstream, 0, 4, aptx->aptx.sampling_freq, aptx_rates);
	print_bitfield8("Channel Mode", bstream, 4, 4, aptx->aptx.channel_mode, aptx_ch_modes);
	print_value32("RFA", bstream += 1, 0, 32, "%#010x", aptx->rfa);
	printf("}\n");

}

static void dump_aptx_ll(const void *blob, size_t size) {

	const a2dp_aptx_ll_t *aptx = blob;
	const a2dp_aptx_ll_new_t *aptx_new_caps = blob;

	size_t conf_size = sizeof(*aptx);
	if (size >= sizeof(*aptx) && aptx->has_new_caps)
		conf_size = sizeof(*aptx_new_caps);
	if (check_blob_size(conf_size, size) == -1)
		return;

	printf("aptX LL (Sprint) <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&aptx->aptx.info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_bitfield8("Sample Rate", bstream, 0, 4, aptx->aptx.sampling_freq, aptx_rates);
	print_bitfield8("Channel Mode", bstream, 4, 4, aptx->aptx.channel_mode, aptx_ch_modes);
	print_value8("RFA", bstream += 1, 0, 6, "%#x", aptx->reserved);
	print_bool8("Has New Capabilities", bstream, 6, 1, aptx->has_new_caps);
	print_bool8("Bidirectional Link", bstream, 7, 1, aptx->bidirect_link);

	if (aptx->has_new_caps) {
		print_value8("RFA", bstream += 1, 0, 8, "%#x", aptx_new_caps->reserved);
		const uint16_t aptx_target_level = A2DP_APTX_LL_GET_TARGET_CODEC_LEVEL(*aptx_new_caps);
		print_value16("Target Codec Level", bstream += 1, 0, 16, "%u", aptx_target_level);
		const uint16_t aptx_initial_level = A2DP_APTX_LL_GET_INITIAL_CODEC_LEVEL(*aptx_new_caps);
		print_value16("Initial Codec Level", bstream += 2, 0, 16, "%u", aptx_initial_level);
		print_value8("SRA Max Rate", bstream += 2, 0, 8, "%u", aptx_new_caps->sra_max_rate);
		print_value8("SRA Avg Time", bstream += 1, 0, 8, "%u", aptx_new_caps->sra_avg_time);
		const uint16_t aptx_working_level = A2DP_APTX_LL_GET_GOOD_WORKING_LEVEL(*aptx_new_caps);
		print_value16("Good Working Level", bstream += 1, 0, 16, "%u", aptx_working_level);
	}

	printf("}\n");

}

static void dump_faststream(const void *blob, size_t size) {

	const a2dp_faststream_t *fs = blob;
	if (check_blob_size(sizeof(*fs), size) == -1)
		return;

	static const struct bitfield directions[] = {
		{ FASTSTREAM_DIRECTION_MUSIC, "Music" },
		{ FASTSTREAM_DIRECTION_VOICE, "Voice" },
		{ 0 },
	};

	static const struct bitfield rates_music[] = {
		{ FASTSTREAM_SAMPLING_FREQ_MUSIC_48000, "48000 Hz" },
		{ FASTSTREAM_SAMPLING_FREQ_MUSIC_44100, "44100 Hz" },
		{ 0 },
	};

	static const struct bitfield rates_voice[] = {
		{ FASTSTREAM_SAMPLING_FREQ_VOICE_16000, "16000 Hz" },
		{ 0 },
	};

	printf("FastStream <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&fs->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_value8("RFA", bstream, 0, 6, "%#x", fs->direction >> 2);
	print_bitfield8("Direction", bstream, 6, 2, fs->direction, directions);
	print_bitfield8("Sample Rate Voice", bstream += 1, 0, 4, fs->sampling_freq_voice, rates_voice);
	print_bitfield8("Sample Rate Music", bstream, 4, 4, fs->sampling_freq_music, rates_music);
	printf("}\n");

}

static void dump_lc3plus(const void *blob, size_t size) {

	const a2dp_lc3plus_t *lc3plus = blob;
	if (check_blob_size(sizeof(*lc3plus), size) == -1)
		return;

	static const struct bitfield durations[] = {
		{ LC3PLUS_FRAME_DURATION_025, "2.5 ms" },
		{ LC3PLUS_FRAME_DURATION_050, "5 ms" },
		{ LC3PLUS_FRAME_DURATION_100, "10 ms" },
		{ 0 },
	};

	static const struct bitfield ch_modes[] = {
		{ LC3PLUS_CHANNEL_MODE_MONO, "Mono" },
		{ LC3PLUS_CHANNEL_MODE_STEREO, "Stereo" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ LC3PLUS_SAMPLING_FREQ_48000, "48000 Hz" },
		{ LC3PLUS_SAMPLING_FREQ_96000, "96000 Hz" },
		{ 0 },
	};

	printf("LC3plus <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&lc3plus->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_bitfield8("Frame Duration", bstream, 0, 4, lc3plus->frame_duration, durations);
	print_value8("RFA", bstream, 4, 4, "%#x", lc3plus->rfa);
	print_bitfield8("Channel Mode", bstream += 1, 0, 8, lc3plus->channel_mode, ch_modes);
	const uint16_t lc3plus_sampling_freq = A2DP_LC3PLUS_GET_SAMPLING_FREQ(*lc3plus);
	print_bitfield16("Sample Rate", bstream += 1, 0, 16, lc3plus_sampling_freq, rates);
	printf("}\n");

}

static void dump_ldac(const void *blob, size_t size) {

	const a2dp_ldac_t *ldac = blob;
	if (check_blob_size(sizeof(*ldac), size) == -1)
		return;

	static const struct bitfield ch_modes[] = {
		{ LDAC_CHANNEL_MODE_MONO, "Mono" },
		{ LDAC_CHANNEL_MODE_DUAL, "Dual Channel" },
		{ LDAC_CHANNEL_MODE_STEREO, "Stereo" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ LDAC_SAMPLING_FREQ_44100, "44100 Hz" },
		{ LDAC_SAMPLING_FREQ_48000, "48000 Hz" },
		{ LDAC_SAMPLING_FREQ_88200, "88200 Hz" },
		{ LDAC_SAMPLING_FREQ_96000, "96000 Hz" },
		{ LDAC_SAMPLING_FREQ_176400, "176400 Hz" },
		{ LDAC_SAMPLING_FREQ_192000, "192000 Hz" },
		{ 0 },
	};

	printf("LDAC <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&ldac->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_value8("RFA", bstream, 0, 2, "%#x", ldac->rfa1);
	print_bitfield8("Sample Rate", bstream, 2, 6, ldac->sampling_freq, rates);
	print_value8("RFA", bstream += 1, 0, 5, "%#x", ldac->rfa2);
	print_bitfield8("Channel Mode", bstream, 5, 3, ldac->channel_mode, ch_modes);
	printf("}\n");

}

static const struct bitfield lhdc_rates[] = {
	{ LHDC_SAMPLING_FREQ_44100, "44100 Hz" },
	{ LHDC_SAMPLING_FREQ_48000, "48000 Hz" },
	{ LHDC_SAMPLING_FREQ_88200, "88200 Hz" },
	{ LHDC_SAMPLING_FREQ_96000, "96000 Hz" },
	{ 0 },
};

static const struct bitfield lhdc_bit_depths[] = {
	{ LHDC_BIT_DEPTH_16, "16 bits" },
	{ LHDC_BIT_DEPTH_24, "24 bits" },
	{ 0 },
};

static const struct bitfield lhdc_versions[] = {
	{ LHDC_VER3, "v3" },
	{ 0 },
};

static const struct bitfield lhdc_ch_split_modes[] = {
	{ LHDC_CH_SPLIT_MODE_NONE, "None" },
	{ LHDC_CH_SPLIT_MODE_TWS, "TWS" },
	{ LHDC_CH_SPLIT_MODE_TWS_PLUS, "TWS+" },
	{ 0 },
};

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
	print_vendor(&lhdc->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_value8("RFA", bstream, 0, 1, "%#x", lhdc->rfa);
	print_bool8("Channel Separation", bstream, 1, 1, lhdc->ch_separation);
	print_bitfield8("Bit Depth", bstream, 2, 2, lhdc->bit_depth, lhdc_bit_depths);
	print_bitfield8("Sample Rate", bstream, 4, 4, lhdc->sampling_freq, lhdc_rates);
	printf("}\n");

}

static void dump_lhdc_v2(const void *blob, size_t size) {

	const a2dp_lhdc_v2_t *lhdc = blob;
	if (check_blob_size(sizeof(*lhdc), size) == -1)
		return;

	printf("LHDC v2 <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&lhdc->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_value8("RFA", bstream, 0, 2, "%#x", lhdc->rfa1);
	print_bitfield8("Bit Depth", bstream, 2, 2, lhdc->bit_depth, lhdc_bit_depths);
	print_bitfield8("Sample Rate", bstream, 4, 4, lhdc->sampling_freq, lhdc_rates);
	print_bool8("Low Latency", bstream += 1, 0, 1, lhdc->low_latency);
	const uint16_t lhdc_max_bitrate = lhdc_get_max_bitrate(lhdc->max_bitrate);
	print_value8("Max Bitrate", bstream, 1, 3, "%#x [%u kbps]", lhdc->max_bitrate, lhdc_max_bitrate);
	print_bitfield8("Version", bstream, 4, 4, lhdc->version, lhdc_versions);
	print_value8("RFA", bstream += 1, 0, 4, "%#x", lhdc->rfa2);
	print_bitfield8("Channel Split Mode", bstream, 4, 4, lhdc->ch_split_mode, lhdc_ch_split_modes);
	printf("}\n");

}

static void dump_lhdc_v3(const void *blob, size_t size) {

	const a2dp_lhdc_v3_t *lhdc = blob;
	if (check_blob_size(sizeof(*lhdc), size) == -1)
		return;

	printf("LHDC v3 <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&lhdc->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_bool8("AR", bstream, 0, 1, lhdc->ar);
	print_bool8("JAS", bstream, 1, 1, lhdc->jas);
	print_bitfield8("Bit Depth", bstream, 2, 2, lhdc->bit_depth, lhdc_bit_depths);
	print_bitfield8("Sample Rate", bstream, 4, 4, lhdc->sampling_freq, lhdc_rates);
	print_bool8("LLAC", bstream += 1, 0, 1, lhdc->llac);
	print_bool8("Low Latency", bstream, 1, 1, lhdc->low_latency);
	const uint16_t lhdc_max_bitrate = lhdc_get_max_bitrate(lhdc->max_bitrate);
	print_value8("Max Bitrate", bstream, 2, 2, "%#x [%u kbps]", lhdc->max_bitrate, lhdc_max_bitrate);
	print_bitfield8("Version", bstream, 4, 4, lhdc->version, lhdc_versions);
	print_bool8("LHDC v4", bstream += 1, 0, 1, lhdc->lhdc_v4);
	print_bool8("LARC", bstream, 1, 1, lhdc->larc);
	print_bool8("Min Bitrate", bstream, 2, 1, lhdc->min_bitrate);
	print_bool8("Meta", bstream, 3, 1, lhdc->meta);
	print_bitfield8("Channel Split Mode", bstream, 4, 4, lhdc->ch_split_mode, lhdc_ch_split_modes);
	printf("}\n");

}

static void dump_lhdc_v5(const void *blob, size_t size) {

	const a2dp_lhdc_v5_t *lhdc = blob;
	if (check_blob_size(sizeof(*lhdc), size) == -1)
		return;

	printf("LHDC v5 <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&lhdc->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_value8("RFA", bstream, 0, 3, "%#x", lhdc->rfa1);
	print_bitfield8("Sample Rate", bstream, 3, 5, lhdc->sampling_freq, lhdc_rates);
	print_value8("Min Bitrate", bstream += 1, 0, 2, "%#x", lhdc->min_bitrate);
	const uint16_t lhdc_max_bitrate = lhdc_get_max_bitrate(lhdc->max_bitrate);
	print_value8("Max Bitrate", bstream, 2, 2, "%#x [%u kbps]", lhdc->max_bitrate, lhdc_max_bitrate);
	print_value8("RFA", bstream, 4, 1, "%#x", lhdc->rfa2);
	print_bitfield8("Bit Depth", bstream, 5, 3, lhdc->bit_depth, lhdc_bit_depths);
	print_value8("RFA", bstream += 1, 0, 3, "%#x", lhdc->rfa3);
	print_bool8("Frame Length 5ms", bstream, 3, 1, lhdc->frame_len_5ms);
	print_bitfield8("Version", bstream, 4, 4, lhdc->version, lhdc_versions);
	print_bool8("RFA", bstream += 1, 0, 1, lhdc->reserved);
	print_bool8("Low Latency", bstream, 1, 1, lhdc->low_latency);
	print_value8("RFA", bstream, 2, 3, "%#x", lhdc->rfa4);
	print_bool8("Meta", bstream, 5, 1, lhdc->meta);
	print_bool8("JAS", bstream, 6, 1, lhdc->jas);
	print_bool8("AR", bstream, 7, 1, lhdc->ar);
	print_value8("RFA", bstream += 1, 0, 7, "%#x", lhdc->rfa5);
	print_bool8("AR On", bstream, 7, 1, lhdc->ar_on);
	printf("}\n");

}

static void dump_opus(const void *blob, size_t size) {

	const a2dp_opus_t *opus = blob;
	if (check_blob_size(sizeof(*opus), size) == -1)
		return;

	static const struct bitfield ch_modes[] = {
		{ OPUS_CHANNEL_MODE_STEREO, "Stereo" },
		{ OPUS_CHANNEL_MODE_DUAL, "Dual Channel" },
		{ OPUS_CHANNEL_MODE_MONO, "Mono" },
		{ 0 },
	};

	static const struct bitfield rates[] = {
		{ OPUS_SAMPLING_FREQ_48000, "48000 Hz" },
		{ OPUS_SAMPLING_FREQ_24000, "24000 Hz" },
		{ OPUS_SAMPLING_FREQ_16000, "16000 Hz" },
		{ 0 },
	};

	static const struct bitfield durations[] = {
		{ OPUS_FRAME_DURATION_100, "10 ms" },
		{ OPUS_FRAME_DURATION_200, "20 ms" },
		{ 0 },
	};

	printf("Opus <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&opus->info);

	const uint8_t *bstream = ((uint8_t *)blob) + sizeof(a2dp_vendor_info_t);
	print_bitfield8("Sample Rate", bstream, 0, 3, opus->sampling_freq, rates);
	print_bitfield8("Frame Duration", bstream, 3, 2, opus->frame_duration, durations);
	print_bitfield8("Channel Mode", bstream, 5, 3, opus->channel_mode, ch_modes);
	printf("}\n");

}

static void dump_opus_pw(const void *blob, size_t size) {

	const a2dp_opus_pw_t *opus = blob;
	if (check_blob_size(sizeof(*opus), size) == -1)
		return;

	static const struct bitfield durations[] = {
		{ OPUS_PW_FRAME_DURATION_025, "2.5 ms" },
		{ OPUS_PW_FRAME_DURATION_050, "5 ms" },
		{ OPUS_PW_FRAME_DURATION_100, "10 ms" },
		{ OPUS_PW_FRAME_DURATION_200, "20 ms" },
		{ OPUS_PW_FRAME_DURATION_400, "40 ms" },
		{ 0 },
	};

	printf("Opus (PipeWire) <hex:%s> {\n", bintohex(blob, size));
	print_vendor(&opus->info);

	const uint8_t *bstream;

	bstream = (uint8_t *)&opus->music;
	print_value8("Music Channels", bstream, 0, 8, "%u", opus->music.channels);
	print_value8("Music Coupled Streams", bstream += 1, 0, 8, "%u", opus->music.coupled_streams);
	const uint32_t opus_music_location = A2DP_OPUS_PW_GET_LOCATION(opus->music);
	print_value32("Music Location", bstream += 1, 0, 32, "%#x", opus_music_location);
	print_bitfield8("Music Frame Duration", bstream += 4, 0, 8, opus->music.frame_duration, durations);
	const uint32_t opus_music_bitrate = A2DP_OPUS_PW_GET_BITRATE(opus->music);
	print_value16("Music Bitrate", bstream += 1, 0, 16, "%u [%u kbps]",
			opus_music_bitrate, opus_music_bitrate * 1024);

	bstream = (uint8_t *)&opus->voice;
	print_value8("Voice Channels", bstream, 0, 8, "%u", opus->voice.channels);
	print_value8("Voice Coupled Streams", bstream += 1, 0, 8, "%u", opus->voice.coupled_streams);
	const uint32_t opus_voice_location = A2DP_OPUS_PW_GET_LOCATION(opus->voice);
	print_value32("Voice Location", bstream += 1, 0, 32, "%#x", opus_voice_location);
	print_bitfield8("Voice Frame Duration", bstream += 4, 0, 8, opus->voice.frame_duration, durations);
	const uint32_t opus_voice_bitrate = A2DP_OPUS_PW_GET_BITRATE(opus->voice);
	print_value16("Voice Bitrate", bstream += 1, 0, 16, "%u [%u kbps]",
			opus_voice_bitrate, opus_voice_bitrate * 1024);

	printf("}\n");

}

static const struct {
	uint32_t codec_id;
	size_t blob_size;
	void (*dump)(const void *, size_t);
} dumps[] = {
	{ A2DP_CODEC_SBC, sizeof(a2dp_sbc_t), dump_sbc },
	{ A2DP_CODEC_MPEG12, sizeof(a2dp_mpeg_t), dump_mpeg },
	{ A2DP_CODEC_MPEG24, sizeof(a2dp_aac_t), dump_aac },
	{ A2DP_CODEC_MPEGD, sizeof(a2dp_usac_t), dump_usac },
	{ A2DP_CODEC_ATRAC, sizeof(a2dp_atrac_t), dump_atrac },
	{ A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
		sizeof(a2dp_aptx_t), dump_aptx },
	{ A2DP_CODEC_VENDOR_ID(APTX_TWS_VENDOR_ID, APTX_TWS_CODEC_ID),
		sizeof(a2dp_aptx_t), dump_aptx_tws },
	{ A2DP_CODEC_VENDOR_ID(APTX_AD_VENDOR_ID, APTX_AD_CODEC_ID),
		sizeof(a2dp_aptx_ad_t), dump_aptx_ad },
	{ A2DP_CODEC_VENDOR_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
		sizeof(a2dp_aptx_hd_t), dump_aptx_hd },
	{ A2DP_CODEC_VENDOR_ID(APTX_LL_VENDOR_ID, APTX_LL_CODEC_ID),
		sizeof(a2dp_aptx_ll_t), dump_aptx_ll },
	{ A2DP_CODEC_VENDOR_ID(APTX_LL_VENDOR_ID, APTX_LL_CODEC_ID),
		sizeof(a2dp_aptx_ll_new_t), dump_aptx_ll },
	{ A2DP_CODEC_VENDOR_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
		sizeof(a2dp_faststream_t), dump_faststream },
	{ A2DP_CODEC_VENDOR_ID(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
		sizeof(a2dp_lc3plus_t), dump_lc3plus },
	{ A2DP_CODEC_VENDOR_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID),
		sizeof(a2dp_ldac_t), dump_ldac },
	{ A2DP_CODEC_VENDOR_ID(LHDC_V1_VENDOR_ID, LHDC_V1_CODEC_ID),
		sizeof(a2dp_lhdc_v1_t), dump_lhdc_v1 },
	{ A2DP_CODEC_VENDOR_ID(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID),
		sizeof(a2dp_lhdc_v2_t), dump_lhdc_v2 },
	{ A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID),
		sizeof(a2dp_lhdc_v3_t), dump_lhdc_v3 },
	{ A2DP_CODEC_VENDOR_ID(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID),
		sizeof(a2dp_lhdc_v5_t), dump_lhdc_v5 },
	{ A2DP_CODEC_VENDOR_ID(LHDC_LL_VENDOR_ID, LHDC_LL_CODEC_ID),
		-1, dump_vendor },
	{ A2DP_CODEC_VENDOR_ID(OPUS_VENDOR_ID, OPUS_CODEC_ID),
		sizeof(a2dp_opus_t), dump_opus },
	{ A2DP_CODEC_VENDOR_ID(OPUS_PW_VENDOR_ID, OPUS_PW_CODEC_ID),
		sizeof(a2dp_opus_pw_t), dump_opus_pw },
	{ A2DP_CODEC_VENDOR_ID(SAMSUNG_HD_VENDOR_ID, SAMSUNG_HD_CODEC_ID),
		-1, dump_vendor },
	{ A2DP_CODEC_VENDOR_ID(SAMSUNG_SC_VENDOR_ID, SAMSUNG_SC_CODEC_ID),
		-1, dump_vendor },
};

int dump(const char *config, bool detect) {

	uint32_t codec_id = get_codec(config);
	int rv = -1;

	ssize_t blob_size;
	if ((blob_size = get_codec_blob(config, NULL, 0)) == -1)
		return -1;

	void *blob = malloc(blob_size);
	if (get_codec_blob(config, blob, blob_size) == -1)
		goto final;

	if (codec_id == A2DP_CODEC_VENDOR &&
			(size_t)blob_size >= sizeof(a2dp_vendor_info_t))
		codec_id = a2dp_codecs_vendor_codec_id(blob);

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
	const char *opts = "hVvx";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
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
					"  -v, --verbose\t\tshow verbose bit-stream details\n"
					"  -x, --auto-detect\ttry to auto-detect codec\n"
					"\nExamples:\n"
					"  %s sbc:ffff0235\n"
					"  %s vendor:4f0000000100ff\n",
					argv[0], argv[0], argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'v' /* --verbose */ :
			verbose = true;
			break;
		case 'x' /* --auto-detect */ :
			detect = true;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (argc - optind < 1)
		goto usage;

	for (int i = optind; i < argc; i++)
		if (dump(argv[i], detect) == -1)
			rv = EXIT_FAILURE;

	return rv;
}
