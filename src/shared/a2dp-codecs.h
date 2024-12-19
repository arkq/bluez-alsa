/*
 * BlueALSA - a2dp-codecs.h
 * Copyright (C) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_A2DPCODECS_H_
#define BLUEALSA_SHARED_A2DPCODECS_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <stdint.h>

#include "bluetooth.h"
#include "defs.h"

#define A2DP_CODEC_SBC      0x00
#define A2DP_CODEC_MPEG12   0x01
#define A2DP_CODEC_MPEG24   0x02
#define A2DP_CODEC_MPEGD    0x03
#define A2DP_CODEC_ATRAC    0x04
#define A2DP_CODEC_VENDOR   0xFF

/**
 * Customized (BlueALSA) 32-bit vendor extension. */
#define A2DP_CODEC_VENDOR_ID(CMP, ID) (((CMP) << 16) | (ID))

#define SBC_SAMPLING_FREQ_16000         (1 << 3)
#define SBC_SAMPLING_FREQ_32000         (1 << 2)
#define SBC_SAMPLING_FREQ_44100         (1 << 1)
#define SBC_SAMPLING_FREQ_48000         (1 << 0)

#define SBC_CHANNEL_MODE_MONO           (1 << 3)
#define SBC_CHANNEL_MODE_DUAL_CHANNEL   (1 << 2)
#define SBC_CHANNEL_MODE_STEREO         (1 << 1)
#define SBC_CHANNEL_MODE_JOINT_STEREO   (1 << 0)

#define SBC_BLOCK_LENGTH_4              (1 << 3)
#define SBC_BLOCK_LENGTH_8              (1 << 2)
#define SBC_BLOCK_LENGTH_12             (1 << 1)
#define SBC_BLOCK_LENGTH_16             (1 << 0)

#define SBC_SUBBANDS_4                  (1 << 1)
#define SBC_SUBBANDS_8                  (1 << 0)

#define SBC_ALLOCATION_SNR              (1 << 1)
#define SBC_ALLOCATION_LOUDNESS         (1 << 0)

#define SBC_MIN_BITPOOL                 2
#define SBC_MAX_BITPOOL                 250

/**
 * Predefined SBC bit-pool values.
 *
 * Other settings:
 *  - block length = 16
 *  - allocation method = Loudness
 *  - sub-bands = 8 */
#define SBC_BITPOOL_LQ_MONO_44100          15
#define SBC_BITPOOL_LQ_MONO_48000          15
#define SBC_BITPOOL_LQ_JOINT_STEREO_44100  29
#define SBC_BITPOOL_LQ_JOINT_STEREO_48000  29
#define SBC_BITPOOL_MQ_MONO_44100          19
#define SBC_BITPOOL_MQ_MONO_48000          18
#define SBC_BITPOOL_MQ_JOINT_STEREO_44100  35
#define SBC_BITPOOL_MQ_JOINT_STEREO_48000  33
#define SBC_BITPOOL_HQ_MONO_44100          31
#define SBC_BITPOOL_HQ_MONO_48000          29
#define SBC_BITPOOL_HQ_JOINT_STEREO_44100  53
#define SBC_BITPOOL_HQ_JOINT_STEREO_48000  51

typedef struct a2dp_sbc {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t sampling_freq:4;
	uint8_t channel_mode:4;
	uint8_t block_length:4;
	uint8_t subbands:2;
	uint8_t allocation_method:2;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t channel_mode:4;
	uint8_t sampling_freq:4;
	uint8_t allocation_method:2;
	uint8_t subbands:2;
	uint8_t block_length:4;
#else
# error "Unknown byte order!"
#endif
	uint8_t min_bitpool;
	uint8_t max_bitpool;
} __attribute__ ((packed)) a2dp_sbc_t;

#define MPEG_CHANNEL_MODE_MONO          (1 << 3)
#define MPEG_CHANNEL_MODE_DUAL_CHANNEL  (1 << 2)
#define MPEG_CHANNEL_MODE_STEREO        (1 << 1)
#define MPEG_CHANNEL_MODE_JOINT_STEREO  (1 << 0)

#define MPEG_LAYER_MP1                  (1 << 2)
#define MPEG_LAYER_MP2                  (1 << 1)
#define MPEG_LAYER_MP3                  (1 << 0)

#define MPEG_SAMPLING_FREQ_16000        (1 << 5)
#define MPEG_SAMPLING_FREQ_22050        (1 << 4)
#define MPEG_SAMPLING_FREQ_24000        (1 << 3)
#define MPEG_SAMPLING_FREQ_32000        (1 << 2)
#define MPEG_SAMPLING_FREQ_44100        (1 << 1)
#define MPEG_SAMPLING_FREQ_48000        (1 << 0)

#define MPEG_BITRATE_INDEX_0            (1 << 0)
#define MPEG_BITRATE_INDEX_1            (1 << 1)
#define MPEG_BITRATE_INDEX_2            (1 << 2)
#define MPEG_BITRATE_INDEX_3            (1 << 3)
#define MPEG_BITRATE_INDEX_4            (1 << 4)
#define MPEG_BITRATE_INDEX_5            (1 << 5)
#define MPEG_BITRATE_INDEX_6            (1 << 6)
#define MPEG_BITRATE_INDEX_7            (1 << 7)
#define MPEG_BITRATE_INDEX_8            (1 << 8)
#define MPEG_BITRATE_INDEX_9            (1 << 9)
#define MPEG_BITRATE_INDEX_10           (1 << 10)
#define MPEG_BITRATE_INDEX_11           (1 << 11)
#define MPEG_BITRATE_INDEX_12           (1 << 12)
#define MPEG_BITRATE_INDEX_13           (1 << 13)
#define MPEG_BITRATE_INDEX_14           (1 << 14)

#define MPEG_MP1_BITRATE_32000          MPEG_BITRATE_INDEX_1
#define MPEG_MP1_BITRATE_64000          MPEG_BITRATE_INDEX_2
#define MPEG_MP1_BITRATE_96000          MPEG_BITRATE_INDEX_3
#define MPEG_MP1_BITRATE_128000         MPEG_BITRATE_INDEX_4
#define MPEG_MP1_BITRATE_160000         MPEG_BITRATE_INDEX_5
#define MPEG_MP1_BITRATE_192000         MPEG_BITRATE_INDEX_6
#define MPEG_MP1_BITRATE_224000         MPEG_BITRATE_INDEX_7
#define MPEG_MP1_BITRATE_256000         MPEG_BITRATE_INDEX_8
#define MPEG_MP1_BITRATE_288000         MPEG_BITRATE_INDEX_9
#define MPEG_MP1_BITRATE_320000         MPEG_BITRATE_INDEX_10
#define MPEG_MP1_BITRATE_352000         MPEG_BITRATE_INDEX_11
#define MPEG_MP1_BITRATE_384000         MPEG_BITRATE_INDEX_12
#define MPEG_MP1_BITRATE_416000         MPEG_BITRATE_INDEX_13
#define MPEG_MP1_BITRATE_448000         MPEG_BITRATE_INDEX_14

#define MPEG_MP2_BITRATE_32000          MPEG_BITRATE_INDEX_1
#define MPEG_MP2_BITRATE_48000          MPEG_BITRATE_INDEX_2
#define MPEG_MP2_BITRATE_56000          MPEG_BITRATE_INDEX_3
#define MPEG_MP2_BITRATE_64000          MPEG_BITRATE_INDEX_4
#define MPEG_MP2_BITRATE_80000          MPEG_BITRATE_INDEX_5
#define MPEG_MP2_BITRATE_96000          MPEG_BITRATE_INDEX_6
#define MPEG_MP2_BITRATE_112000         MPEG_BITRATE_INDEX_7
#define MPEG_MP2_BITRATE_128000         MPEG_BITRATE_INDEX_8
#define MPEG_MP2_BITRATE_160000         MPEG_BITRATE_INDEX_9
#define MPEG_MP2_BITRATE_192000         MPEG_BITRATE_INDEX_10
#define MPEG_MP2_BITRATE_224000         MPEG_BITRATE_INDEX_11
#define MPEG_MP2_BITRATE_256000         MPEG_BITRATE_INDEX_12
#define MPEG_MP2_BITRATE_320000         MPEG_BITRATE_INDEX_13
#define MPEG_MP2_BITRATE_384000         MPEG_BITRATE_INDEX_14

#define MPEG_MP3_BITRATE_32000          MPEG_BITRATE_INDEX_1
#define MPEG_MP3_BITRATE_40000          MPEG_BITRATE_INDEX_2
#define MPEG_MP3_BITRATE_48000          MPEG_BITRATE_INDEX_3
#define MPEG_MP3_BITRATE_56000          MPEG_BITRATE_INDEX_4
#define MPEG_MP3_BITRATE_64000          MPEG_BITRATE_INDEX_5
#define MPEG_MP3_BITRATE_80000          MPEG_BITRATE_INDEX_6
#define MPEG_MP3_BITRATE_96000          MPEG_BITRATE_INDEX_7
#define MPEG_MP3_BITRATE_112000         MPEG_BITRATE_INDEX_8
#define MPEG_MP3_BITRATE_128000         MPEG_BITRATE_INDEX_9
#define MPEG_MP3_BITRATE_160000         MPEG_BITRATE_INDEX_10
#define MPEG_MP3_BITRATE_192000         MPEG_BITRATE_INDEX_11
#define MPEG_MP3_BITRATE_224000         MPEG_BITRATE_INDEX_12
#define MPEG_MP3_BITRATE_256000         MPEG_BITRATE_INDEX_13
#define MPEG_MP3_BITRATE_320000         MPEG_BITRATE_INDEX_14

#define MPEG_BITRATE_FREE               MPEG_BITRATE_INDEX_0

typedef struct a2dp_mpeg {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t layer:3;
	uint8_t crc:1;
	uint8_t channel_mode:4;
	uint8_t rfa:1;
	uint8_t mpf:1;
	uint8_t sampling_freq:6;
	uint8_t vbr:1;
	uint8_t bitrate1:7;
	uint8_t bitrate2;
#else
	uint8_t channel_mode:4;
	uint8_t crc:1;
	uint8_t layer:3;
	uint8_t sampling_freq:6;
	uint8_t mpf:1;
	uint8_t rfa:1;
	uint8_t bitrate1:7;
	uint8_t vbr:1;
	uint8_t bitrate2;
#endif

#define A2DP_MPEG_INIT_BITRATE(v) \
	.bitrate1 = ((v) >> 8) & 0x7F, \
	.bitrate2 = (v) & 0xFF,
#define A2DP_MPEG_GET_BITRATE(a) \
	((a).bitrate1 << 8 | (a).bitrate2)

} __attribute__ ((packed)) a2dp_mpeg_t;

#define AAC_OBJECT_TYPE_MPEG2_LC        (1 << 6) /* Low Complexity MPEG-2 */
#define AAC_OBJECT_TYPE_MPEG4_LC        (1 << 5) /* Low Complexity */
#define AAC_OBJECT_TYPE_MPEG4_LTP       (1 << 4) /* Long Term Prediction */
#define AAC_OBJECT_TYPE_MPEG4_SCA       (1 << 3) /* Scalable */
#define AAC_OBJECT_TYPE_MPEG4_HE        (1 << 2) /* High Efficiency */
#define AAC_OBJECT_TYPE_MPEG4_HE2       (1 << 1) /* High Efficiency v2 */
#define AAC_OBJECT_TYPE_MPEG4_ELD2      (1 << 0) /* Enhanced Low Delay */

#define AAC_SAMPLING_FREQ_8000          (1 << 11)
#define AAC_SAMPLING_FREQ_11025         (1 << 10)
#define AAC_SAMPLING_FREQ_12000         (1 << 9)
#define AAC_SAMPLING_FREQ_16000         (1 << 8)
#define AAC_SAMPLING_FREQ_22050         (1 << 7)
#define AAC_SAMPLING_FREQ_24000         (1 << 6)
#define AAC_SAMPLING_FREQ_32000         (1 << 5)
#define AAC_SAMPLING_FREQ_44100         (1 << 4)
#define AAC_SAMPLING_FREQ_48000         (1 << 3)
#define AAC_SAMPLING_FREQ_64000         (1 << 2)
#define AAC_SAMPLING_FREQ_88200         (1 << 1)
#define AAC_SAMPLING_FREQ_96000         (1 << 0)

#define AAC_CHANNEL_MODE_MONO           (1 << 3)
#define AAC_CHANNEL_MODE_STEREO         (1 << 2)
#define AAC_CHANNEL_MODE_5_1            (1 << 1)
#define AAC_CHANNEL_MODE_7_1            (1 << 0)

typedef struct a2dp_aac {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t object_type:7;
	uint8_t drc:1;
	uint8_t sampling_freq1;
	uint8_t sampling_freq2:4;
	uint8_t channel_mode:4;
	uint8_t vbr:1;
	uint8_t bitrate1:7;
	uint8_t bitrate2;
	uint8_t bitrate3;
#else
	uint8_t drc:1;
	uint8_t object_type:7;
	uint8_t sampling_freq1;
	uint8_t channel_mode:4;
	uint8_t sampling_freq2:4;
	uint8_t bitrate1:7;
	uint8_t vbr:1;
	uint8_t bitrate2;
	uint8_t bitrate3;
#endif

#define A2DP_AAC_INIT_BITRATE(v) \
	.bitrate1 = ((v) >> 16) & 0x7F, \
	.bitrate2 = ((v) >> 8) & 0xFF, \
	.bitrate3 = (v) & 0xff,
#define A2DP_AAC_GET_BITRATE(a) \
	((a).bitrate1 << 16 | (a).bitrate2 << 8 | (a).bitrate3)
#define A2DP_AAC_SET_BITRATE(a, v) do { \
		(a).bitrate1 = ((v) >> 16) & 0x7F; \
		(a).bitrate2 = ((v) >> 8) & 0xFF; \
		(a).bitrate3 = (v) & 0xFF; \
	} while (0)

#define A2DP_AAC_INIT_SAMPLING_FREQ(v) \
	.sampling_freq1 = ((v) >> 4) & 0xFF, \
	.sampling_freq2 = (v) & 0x0F,
#define A2DP_AAC_GET_SAMPLING_FREQ(a) \
	((a).sampling_freq1 << 4 | (a).sampling_freq2)
#define A2DP_AAC_SET_SAMPLING_FREQ(a, v) do { \
		(a).sampling_freq1 = ((v) >> 4) & 0xFF; \
		(a).sampling_freq2 = (v) & 0x0F; \
	} while (0)

} __attribute__ ((packed)) a2dp_aac_t;

#define USAC_OBJECT_TYPE_MPEGD_DRC      (1 << 1)

#define USAC_SAMPLING_FREQ_7350         (1 << 25)
#define USAC_SAMPLING_FREQ_8000         (1 << 24)
#define USAC_SAMPLING_FREQ_8820         (1 << 23)
#define USAC_SAMPLING_FREQ_9600         (1 << 22)
#define USAC_SAMPLING_FREQ_11025        (1 << 21)
#define USAC_SAMPLING_FREQ_11760        (1 << 20)
#define USAC_SAMPLING_FREQ_12000        (1 << 19)
#define USAC_SAMPLING_FREQ_12800        (1 << 18)
#define USAC_SAMPLING_FREQ_14700        (1 << 17)
#define USAC_SAMPLING_FREQ_16000        (1 << 16)
#define USAC_SAMPLING_FREQ_17640        (1 << 15)
#define USAC_SAMPLING_FREQ_19200        (1 << 14)
#define USAC_SAMPLING_FREQ_22050        (1 << 13)
#define USAC_SAMPLING_FREQ_24000        (1 << 12)
#define USAC_SAMPLING_FREQ_29400        (1 << 11)
#define USAC_SAMPLING_FREQ_32000        (1 << 10)
#define USAC_SAMPLING_FREQ_35280        (1 << 9)
#define USAC_SAMPLING_FREQ_38400        (1 << 8)
#define USAC_SAMPLING_FREQ_44100        (1 << 7)
#define USAC_SAMPLING_FREQ_48000        (1 << 6)
#define USAC_SAMPLING_FREQ_58800        (1 << 5)
#define USAC_SAMPLING_FREQ_64000        (1 << 4)
#define USAC_SAMPLING_FREQ_70560        (1 << 3)
#define USAC_SAMPLING_FREQ_76800        (1 << 2)
#define USAC_SAMPLING_FREQ_88200        (1 << 1)
#define USAC_SAMPLING_FREQ_96000        (1 << 0)

#define USAC_CHANNEL_MODE_MONO          (1 << 3)
#define USAC_CHANNEL_MODE_STEREO        (1 << 2)

typedef struct a2dp_usac {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t object_type:2;
	uint8_t sampling_freq1:6;
	uint8_t sampling_freq2;
	uint8_t sampling_freq3;
	uint8_t sampling_freq4:4;
	uint8_t channel_mode:4;
	uint8_t vbr:1;
	uint8_t bitrate1:7;
	uint8_t bitrate2;
	uint8_t bitrate3;
#else
	uint8_t sampling_freq1:6;
	uint8_t object_type:2;
	uint8_t sampling_freq2;
	uint8_t sampling_freq3;
	uint8_t channel_mode:4;
	uint8_t sampling_freq4:4;
	uint8_t bitrate1:7;
	uint8_t vbr:1;
	uint8_t bitrate2;
	uint8_t bitrate3;
#endif

#define A2DP_USAC_GET_BITRATE(a) \
	((a).bitrate1 << 16 | (a).bitrate2 << 8 | (a).bitrate3)
#define A2DP_USAC_GET_SAMPLING_FREQ(a) ( \
		(a).sampling_freq1 << 20 | (a).sampling_freq2 << 12 | \
		(a).sampling_freq3 << 4 | (a).sampling_freq4)

} __attribute__ ((packed)) a2dp_usac_t;

#define ATRAC_CHANNEL_MODE_MONO         (1 << 2)
#define ATRAC_CHANNEL_MODE_DUAL_CHANNEL (1 << 1)
#define ATRAC_CHANNEL_MODE_JOINT_STEREO (1 << 0)

#define ATRAC_SAMPLING_FREQ_44100       (1 << 1)
#define ATRAC_SAMPLING_FREQ_48000       (1 << 0)

typedef struct a2dp_atrac {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t version:3;
	uint8_t channel_mode:3;
	uint8_t rfa1:2;
	uint8_t rfa2:2;
	uint8_t sampling_freq:2;
	uint8_t vbr:1;
	uint8_t bitrate1:3;
	uint8_t bitrate2;
	uint8_t bitrate3;
#else
	uint8_t rfa1:2;
	uint8_t channel_mode:3;
	uint8_t version:3;
	uint8_t bitrate1:3;
	uint8_t vbr:1;
	uint8_t sampling_freq:2;
	uint8_t rfa2:2;
	uint8_t bitrate2;
	uint8_t bitrate3;
#endif
	uint16_t max_sul;
	uint8_t rfa3;

#define A2DP_ATRAC_GET_BITRATE(a) \
	((a).bitrate1 << 16 | (a).bitrate2 << 8 | (a).bitrate3)
#define A2DP_ATRAC_GET_MAX_SUL(a) be16toh((a).max_sul)

} __attribute__ ((packed)) a2dp_atrac_t;

typedef struct a2dp_vendor_info {
	uint32_t vendor_id;
	uint16_t codec_id;
#define A2DP_VENDOR_INFO_INIT(v, c) { HTOLE32(v), HTOLE16(c) }
#define A2DP_VENDOR_INFO_GET_VENDOR_ID(a) le32toh((a).vendor_id)
#define A2DP_VENDOR_INFO_GET_CODEC_ID(a) le16toh((a).codec_id)
} __attribute__ ((packed)) a2dp_vendor_info_t;

#define APTX_VENDOR_ID                  BT_COMPID_APT
#define APTX_CODEC_ID                   0x0001

#define APTX_CHANNEL_MODE_MONO          (1 << 0)
#define APTX_CHANNEL_MODE_STEREO        (1 << 1)
#define APTX_CHANNEL_MODE_TWS           (1 << 3)

#define APTX_SAMPLING_FREQ_16000        (1 << 3)
#define APTX_SAMPLING_FREQ_32000        (1 << 2)
#define APTX_SAMPLING_FREQ_44100        (1 << 1)
#define APTX_SAMPLING_FREQ_48000        (1 << 0)

typedef struct a2dp_aptx {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t sampling_freq:4;
	uint8_t channel_mode:4;
#else
	uint8_t channel_mode:4;
	uint8_t sampling_freq:4;
#endif
} __attribute__ ((packed)) a2dp_aptx_t;

#define FASTSTREAM_VENDOR_ID            BT_COMPID_QUALCOMM_TECH_INTL
#define FASTSTREAM_CODEC_ID             0x0001

#define FASTSTREAM_DIRECTION_VOICE      (1 << 1)
#define FASTSTREAM_DIRECTION_MUSIC      (1 << 0)

#define FASTSTREAM_SAMPLING_FREQ_MUSIC_44100  (1 << 1)
#define FASTSTREAM_SAMPLING_FREQ_MUSIC_48000  (1 << 0)

#define FASTSTREAM_SAMPLING_FREQ_VOICE_16000  (1 << 1)

typedef struct a2dp_faststream {
	a2dp_vendor_info_t info;
	uint8_t direction;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t sampling_freq_voice:4;
	uint8_t sampling_freq_music:4;
#else
	uint8_t sampling_freq_music:4;
	uint8_t sampling_freq_voice:4;
#endif
} __attribute__ ((packed)) a2dp_faststream_t;

#define APTX_LL_VENDOR_ID               BT_COMPID_QUALCOMM_TECH_INTL
#define APTX_LL_CODEC_ID                0x0002

/**
 * Default parameters for aptX LL (Sprint) encoder */
#define APTX_LL_TARGET_CODEC_LEVEL      180  /* target codec buffer level */
#define APTX_LL_INITIAL_CODEC_LEVEL     360  /* initial codec buffer level */
#define APTX_LL_SRA_MAX_RATE            50   /* x/10000 = 0.005 SRA rate */
#define APTX_LL_SRA_AVG_TIME            1    /* SRA averaging time = 1s */
#define APTX_LL_GOOD_WORKING_LEVEL      180  /* good working buffer level */

typedef struct a2dp_aptx_ll {
	a2dp_aptx_t aptx;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t reserved:6;
	uint8_t has_new_caps:1;
	uint8_t bidirect_link:1;
#else
	uint8_t bidirect_link:1;
	uint8_t has_new_caps:1;
	uint8_t reserved:6;
#endif
} __attribute__ ((packed)) a2dp_aptx_ll_t;

typedef struct a2dp_aptx_ll_new {
	a2dp_aptx_ll_t aptx_ll;
	uint8_t reserved;
	uint16_t target_codec_level;
	uint16_t initial_codec_level;
	uint8_t sra_max_rate;
	uint8_t sra_avg_time;
	uint16_t good_working_level;
#define A2DP_APTX_LL_GET_TARGET_CODEC_LEVEL(a) le16toh((a).target_codec_level)
#define A2DP_APTX_LL_GET_INITIAL_CODEC_LEVEL(a) le16toh((a).initial_codec_level)
#define A2DP_APTX_LL_GET_GOOD_WORKING_LEVEL(a) le16toh((a).good_working_level)
} __attribute__ ((packed)) a2dp_aptx_ll_new_t;

#define APTX_HD_VENDOR_ID               BT_COMPID_QUALCOMM_TECH
#define APTX_HD_CODEC_ID                0x0024

typedef struct a2dp_aptx_hd {
	a2dp_aptx_t aptx;
	uint32_t rfa;
} __attribute__ ((packed)) a2dp_aptx_hd_t;

#define APTX_TWS_VENDOR_ID              BT_COMPID_QUALCOMM_TECH
#define APTX_TWS_CODEC_ID               0x0025

#define APTX_AD_VENDOR_ID               BT_COMPID_QUALCOMM_TECH
#define APTX_AD_CODEC_ID                0x00AD

#define APTX_AD_CHANNEL_MODE_MONO             (1 << 0)
#define APTX_AD_CHANNEL_MODE_STEREO           (1 << 1)
#define APTX_AD_CHANNEL_MODE_TWS              (1 << 2)
#define APTX_AD_CHANNEL_MODE_JOINT_STEREO     (1 << 3)
#define APTX_AD_CHANNEL_MODE_TWS_MONO         (1 << 4)

#define APTX_AD_SAMPLING_FREQ_44100     (1 << 0)
#define APTX_AD_SAMPLING_FREQ_48000     (1 << 1)
#define APTX_AD_SAMPLING_FREQ_88000     (1 << 2)
#define APTX_AD_SAMPLING_FREQ_192000    (1 << 3)

typedef struct {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t sampling_freq:5;
	uint8_t rfa1:3;
	uint8_t rfa2:3;
	uint8_t channel_mode:5;
#else
	uint8_t rfa1:3;
	uint8_t sampling_freq:5;
	uint8_t channel_mode:5;
	uint8_t rfa2:3;
#endif
	uint8_t ttp_ll_low;
	uint8_t ttp_ll_high;
	uint8_t ttp_hq_low;
	uint8_t ttp_hq_high;
	uint8_t ttp_tws_low;
	uint8_t ttp_tws_high;
	uint8_t eoc[3];
} __attribute__ ((packed)) a2dp_aptx_ad_t;

#define LC3PLUS_VENDOR_ID               BT_COMPID_FRAUNHOFER_IIS
#define LC3PLUS_CODEC_ID                0x0001

#define LC3PLUS_FRAME_DURATION_025      (1 << 0)
#define LC3PLUS_FRAME_DURATION_050      (1 << 1)
#define LC3PLUS_FRAME_DURATION_100      (1 << 2)

#define LC3PLUS_CHANNEL_MODE_MONO       (1 << 7)
#define LC3PLUS_CHANNEL_MODE_STEREO     (1 << 6)

#define LC3PLUS_SAMPLING_FREQ_48000     (1 << 8)
#define LC3PLUS_SAMPLING_FREQ_96000     (1 << 7)

typedef struct a2dp_lc3plus {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t frame_duration:4;
	uint8_t rfa:4;
#else
	uint8_t rfa:4;
	uint8_t frame_duration:4;
#endif
	uint8_t channel_mode;
	uint16_t sampling_freq12;
#define A2DP_LC3PLUS_INIT_SAMPLING_FREQ(v) .sampling_freq12 = HTOBE16(v),
#define A2DP_LC3PLUS_GET_SAMPLING_FREQ(a) be16toh((a).sampling_freq12)
#define A2DP_LC3PLUS_SET_SAMPLING_FREQ(a, v) ((a).sampling_freq12 = htobe16(v))
} __attribute__ ((packed)) a2dp_lc3plus_t;

#define LDAC_VENDOR_ID                  BT_COMPID_SONY
#define LDAC_CODEC_ID                   0x00AA

#define LDAC_SAMPLING_FREQ_44100        (1 << 5)
#define LDAC_SAMPLING_FREQ_48000        (1 << 4)
#define LDAC_SAMPLING_FREQ_88200        (1 << 3)
#define LDAC_SAMPLING_FREQ_96000        (1 << 2)
#define LDAC_SAMPLING_FREQ_176400       (1 << 1)
#define LDAC_SAMPLING_FREQ_192000       (1 << 0)

#define LDAC_CHANNEL_MODE_MONO          (1 << 2)
#define LDAC_CHANNEL_MODE_DUAL          (1 << 1)
#define LDAC_CHANNEL_MODE_STEREO        (1 << 0)

typedef struct a2dp_ldac {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t rfa1:2;
	uint8_t sampling_freq:6;
	uint8_t rfa2:5;
	uint8_t channel_mode:3;
#else
	uint8_t sampling_freq:6;
	uint8_t rfa1:2;
	uint8_t channel_mode:3;
	uint8_t rfa2:5;
#endif
} __attribute__ ((packed)) a2dp_ldac_t;

#define LHDC_V1_VENDOR_ID               BT_COMPID_SAVITECH
#define LHDC_V1_CODEC_ID                0x484C

#define LHDC_V2_VENDOR_ID               BT_COMPID_SAVITECH
#define LHDC_V2_CODEC_ID                0x4C32

#define LHDC_V3_VENDOR_ID               BT_COMPID_SAVITECH
#define LHDC_V3_CODEC_ID                0x4C33

#define LHDC_V5_VENDOR_ID               BT_COMPID_SAVITECH
#define LHDC_V5_CODEC_ID                0x4C35

#define LHDC_LL_VENDOR_ID               BT_COMPID_SAVITECH
#define LHDC_LL_CODEC_ID                0x4C4C

#define LHDC_BIT_DEPTH_16               (1 << 1)
#define LHDC_BIT_DEPTH_24               (1 << 0)

#define LHDC_SAMPLING_FREQ_44100        (1 << 3)
#define LHDC_SAMPLING_FREQ_48000        (1 << 2)
#define LHDC_SAMPLING_FREQ_88200        (1 << 1)
#define LHDC_SAMPLING_FREQ_96000        (1 << 0)

#define LHDC_MAX_BITRATE_400K           (1 << 1)
#define LHDC_MAX_BITRATE_500K           (1 << 0)
#define LHDC_MAX_BITRATE_900K           (0)

#define LHDC_CH_SPLIT_MODE_NONE         (1 << 0)
#define LHDC_CH_SPLIT_MODE_TWS          (1 << 1)
#define LHDC_CH_SPLIT_MODE_TWS_PLUS     (1 << 2)

#define LHDC_VER3                       (1 << 0)

typedef struct a2dp_lhdc_v1 {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t rfa:1;
	uint8_t ch_separation:1;
	uint8_t bit_depth:2;
	uint8_t sampling_freq:4;
#else
	uint8_t sampling_freq:4;
	uint8_t bit_depth:2;
	uint8_t ch_separation:1;
	uint8_t rfa:1;
#endif
} __attribute__ ((packed)) a2dp_lhdc_v1_t;

typedef struct a2dp_lhdc_v2 {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t rfa1:2;
	uint8_t bit_depth:2;
	uint8_t sampling_freq:4;
	uint8_t low_latency:1;
	uint8_t max_bitrate:3;
	uint8_t version:4;
	uint8_t rfa2:4;
	uint8_t ch_split_mode:4;
#else
	uint8_t sampling_freq:4;
	uint8_t bit_depth:2;
	uint8_t rfa1:2;
	uint8_t version:4;
	uint8_t max_bitrate:3;
	uint8_t low_latency:1;
	uint8_t ch_split_mode:4;
	uint8_t rfa2:4;
#endif
} __attribute__ ((packed)) a2dp_lhdc_v2_t;

typedef struct a2dp_lhdc_v3 {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t ar:1;
	uint8_t jas:1;
	uint8_t bit_depth:2;
	uint8_t sampling_freq:4;
	uint8_t llac:1;
	uint8_t low_latency:1;
	uint8_t max_bitrate:2;
	uint8_t version:4;
	uint8_t lhdc_v4:1;
	uint8_t larc:1;
	uint8_t min_bitrate:1;
	uint8_t meta:1;
	uint8_t ch_split_mode:4;
#else
	uint8_t sampling_freq:4;
	uint8_t bit_depth:2;
	uint8_t jas:1;
	uint8_t ar:1;
	uint8_t version:4;
	uint8_t max_bitrate:2;
	uint8_t low_latency:1;
	uint8_t llac:1;
	uint8_t ch_split_mode:4;
	uint8_t meta:1;
	uint8_t min_bitrate:1;
	uint8_t larc:1;
	uint8_t lhdc_v4:1;
#endif
} __attribute__ ((packed)) a2dp_lhdc_v3_t;

typedef struct a2dp_lhdc_v5 {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t rfa1:3;
	uint8_t sampling_freq:5;
	uint8_t min_bitrate:2;
	uint8_t max_bitrate:2;
	uint8_t rfa2:1;
	uint8_t bit_depth:3;
	uint8_t rfa3:3;
	uint8_t frame_len_5ms:1;
	uint8_t version:4;
	uint8_t reserved:1; /* TODO: lossless? */
	uint8_t low_latency:1;
	uint8_t rfa4:3;
	uint8_t meta:1;
	uint8_t jas:1;
	uint8_t ar:1;
	uint8_t rfa5:7;
	uint8_t ar_on:1;
#else
	uint8_t sampling_freq:5;
	uint8_t rfa1:3;
	uint8_t bit_depth:3;
	uint8_t rfa2:1;
	uint8_t max_bitrate:2;
	uint8_t min_bitrate:2;
	uint8_t version:4;
	uint8_t frame_len_5ms:1;
	uint8_t rfa3:3;
	uint8_t ar:1;
	uint8_t jas:1;
	uint8_t meta:1;
	uint8_t rfa4:3;
	uint8_t low_latency:1;
	uint8_t reserved:1; /* TODO: lossless? */
	uint8_t ar_on:1;
	uint8_t rfa5:7;
#endif
} __attribute__ ((packed)) a2dp_lhdc_v5_t;

#define OPUS_VENDOR_ID                  BT_COMPID_GOOGLE
#define OPUS_CODEC_ID                   0x0001

#define OPUS_SAMPLING_FREQ_48000        (1 << 2)
#define OPUS_SAMPLING_FREQ_24000        (1 << 1)
#define OPUS_SAMPLING_FREQ_16000        (1 << 0)

#define OPUS_FRAME_DURATION_100         (1 << 0)
#define OPUS_FRAME_DURATION_200         (1 << 1)

#define OPUS_CHANNEL_MODE_MONO          (1 << 0)
#define OPUS_CHANNEL_MODE_STEREO        (1 << 1)
#define OPUS_CHANNEL_MODE_DUAL          (1 << 2)

typedef struct a2dp_opus {
	a2dp_vendor_info_t info;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t sampling_freq:3;
	uint8_t frame_duration:2;
	uint8_t channel_mode:3;
#else
	uint8_t channel_mode:3;
	uint8_t frame_duration:2;
	uint8_t sampling_freq:3;
#endif
} __attribute__ ((packed)) a2dp_opus_t;

#define OPUS_PW_VENDOR_ID               BT_COMPID_LINUX_FOUNDATION
#define OPUS_PW_CODEC_ID                0x1005

#define OPUS_PW_FRAME_DURATION_025      (1 << 0)
#define OPUS_PW_FRAME_DURATION_050      (1 << 1)
#define OPUS_PW_FRAME_DURATION_100      (1 << 2)
#define OPUS_PW_FRAME_DURATION_200      (1 << 3)
#define OPUS_PW_FRAME_DURATION_400      (1 << 4)

typedef struct a2dp_opus_pw_stream {
	uint8_t channels;
	uint8_t coupled_streams;
	uint32_t location;
	uint8_t frame_duration;
	uint16_t bitrate;

#define A2DP_OPUS_PW_INIT_LOCATION(v) .location = HTOLE32(v),
#define A2DP_OPUS_PW_GET_LOCATION(a) le32toh((a).location)
#define A2DP_OPUS_PW_SET_LOCATION(a, v) ((a).location = htole32(v))

#define A2DP_OPUS_PW_INIT_BITRATE(v) .bitrate = HTOLE16(v),
#define A2DP_OPUS_PW_GET_BITRATE(a) le32toh((a).bitrate)
#define A2DP_OPUS_PW_SET_BITRATE(a, v) ((a).bitrate = htole16(v))

} __attribute__ ((packed)) a2dp_opus_pw_stream_t;

typedef struct a2dp_opus_pw {
	a2dp_vendor_info_t info;
	a2dp_opus_pw_stream_t music;
	a2dp_opus_pw_stream_t voice;
} __attribute__ ((packed)) a2dp_opus_pw_t;

#define SAMSUNG_HD_VENDOR_ID            BT_COMPID_SAMSUNG_ELEC
#define SAMSUNG_HD_CODEC_ID             0x0102

#define SAMSUNG_SC_VENDOR_ID            BT_COMPID_SAMSUNG_ELEC
#define SAMSUNG_SC_CODEC_ID             0x0103

/**
 * Type big enough to hold any A2DP codec configuration. */
typedef union a2dp {
	a2dp_sbc_t sbc;
	a2dp_mpeg_t mpeg;
	a2dp_aac_t aac;
	a2dp_usac_t usac;
	a2dp_atrac_t atrac;
	a2dp_faststream_t faststream;
	a2dp_aptx_t aptx;
	a2dp_aptx_ad_t aptx_ad;
	a2dp_aptx_hd_t aptx_hd;
	a2dp_aptx_ll_t aptx_ll;
	a2dp_aptx_ll_new_t aptx_ll_new;
	a2dp_lc3plus_t lc3plus;
	a2dp_ldac_t ldac;
	a2dp_lhdc_v1_t lhdc_v1;
	a2dp_lhdc_v2_t lhdc_v2;
	a2dp_lhdc_v3_t lhdc_v3;
	a2dp_lhdc_v5_t lhdc_v5;
	a2dp_opus_t opus;
	a2dp_opus_pw_t opus_pw;
} a2dp_t;

uint32_t a2dp_codecs_codec_id_from_string(const char *alias);
uint32_t a2dp_codecs_vendor_codec_id(const a2dp_vendor_info_t *info);
const char *a2dp_codecs_codec_id_to_string(uint32_t codec_id);
const char *a2dp_codecs_get_canonical_name(const char *alias);

#endif
