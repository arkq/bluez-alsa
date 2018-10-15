/*
 * BlueALSA - bluez-a2dp.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "bluez-a2dp.h"
#include "shared/defs.h"

static const a2dp_sbc_t a2dp_sbc = {
	.frequency =
		SBC_SAMPLING_FREQ_16000 |
		SBC_SAMPLING_FREQ_32000 |
		SBC_SAMPLING_FREQ_44100 |
		SBC_SAMPLING_FREQ_48000,
	.channel_mode =
		SBC_CHANNEL_MODE_MONO |
		SBC_CHANNEL_MODE_DUAL_CHANNEL |
		SBC_CHANNEL_MODE_STEREO |
		SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length =
		SBC_BLOCK_LENGTH_4 |
		SBC_BLOCK_LENGTH_8 |
		SBC_BLOCK_LENGTH_12 |
		SBC_BLOCK_LENGTH_16,
	.subbands =
		SBC_SUBBANDS_4 |
		SBC_SUBBANDS_8,
	.allocation_method =
		SBC_ALLOCATION_SNR |
		SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

static const struct bluez_a2dp_channel_mode a2dp_sbc_channels[] = {
	{ BLUEZ_A2DP_CHM_MONO, SBC_CHANNEL_MODE_MONO },
	{ BLUEZ_A2DP_CHM_DUAL_CHANNEL, SBC_CHANNEL_MODE_DUAL_CHANNEL },
	{ BLUEZ_A2DP_CHM_STEREO, SBC_CHANNEL_MODE_STEREO },
	{ BLUEZ_A2DP_CHM_JOINT_STEREO, SBC_CHANNEL_MODE_JOINT_STEREO },
};

static const struct bluez_a2dp_sampling_freq a2dp_sbc_samplings[] = {
	{ 16000, SBC_SAMPLING_FREQ_16000 },
	{ 32000, SBC_SAMPLING_FREQ_32000 },
	{ 44100, SBC_SAMPLING_FREQ_44100 },
	{ 48000, SBC_SAMPLING_FREQ_48000 },
};

static const a2dp_mpeg_t a2dp_mpeg = {
	.layer =
		MPEG_LAYER_MP1 |
		MPEG_LAYER_MP2 |
		MPEG_LAYER_MP3,
	.crc = 1,
	.channel_mode =
		MPEG_CHANNEL_MODE_MONO |
		MPEG_CHANNEL_MODE_DUAL_CHANNEL |
		MPEG_CHANNEL_MODE_STEREO |
		MPEG_CHANNEL_MODE_JOINT_STEREO,
	.mpf = 1,
	.frequency =
		MPEG_SAMPLING_FREQ_16000 |
		MPEG_SAMPLING_FREQ_22050 |
		MPEG_SAMPLING_FREQ_24000 |
		MPEG_SAMPLING_FREQ_32000 |
		MPEG_SAMPLING_FREQ_44100 |
		MPEG_SAMPLING_FREQ_48000,
	.bitrate =
		MPEG_BIT_RATE_VBR |
		MPEG_BIT_RATE_320000 |
		MPEG_BIT_RATE_256000 |
		MPEG_BIT_RATE_224000 |
		MPEG_BIT_RATE_192000 |
		MPEG_BIT_RATE_160000 |
		MPEG_BIT_RATE_128000 |
		MPEG_BIT_RATE_112000 |
		MPEG_BIT_RATE_96000 |
		MPEG_BIT_RATE_80000 |
		MPEG_BIT_RATE_64000 |
		MPEG_BIT_RATE_56000 |
		MPEG_BIT_RATE_48000 |
		MPEG_BIT_RATE_40000 |
		MPEG_BIT_RATE_32000 |
		MPEG_BIT_RATE_FREE,
};

static const struct bluez_a2dp_channel_mode a2dp_mpeg_channels[] = {
	{ BLUEZ_A2DP_CHM_MONO, MPEG_CHANNEL_MODE_MONO },
	{ BLUEZ_A2DP_CHM_DUAL_CHANNEL, MPEG_CHANNEL_MODE_DUAL_CHANNEL },
	{ BLUEZ_A2DP_CHM_STEREO, MPEG_CHANNEL_MODE_STEREO },
	{ BLUEZ_A2DP_CHM_JOINT_STEREO, MPEG_CHANNEL_MODE_JOINT_STEREO },
};

static const struct bluez_a2dp_sampling_freq a2dp_mpeg_samplings[] = {
	{ 16000, MPEG_SAMPLING_FREQ_16000 },
	{ 22050, MPEG_SAMPLING_FREQ_22050 },
	{ 24000, MPEG_SAMPLING_FREQ_24000 },
	{ 32000, MPEG_SAMPLING_FREQ_32000 },
	{ 44100, MPEG_SAMPLING_FREQ_44100 },
	{ 48000, MPEG_SAMPLING_FREQ_48000 },
};

static const a2dp_aac_t a2dp_aac = {
	.object_type =
		/* NOTE: AAC Long Term Prediction and AAC Scalable are
		 *       not supported by the FDK-AAC library. */
		AAC_OBJECT_TYPE_MPEG2_AAC_LC |
		AAC_OBJECT_TYPE_MPEG4_AAC_LC,
	AAC_INIT_FREQUENCY(
			AAC_SAMPLING_FREQ_8000 |
			AAC_SAMPLING_FREQ_11025 |
			AAC_SAMPLING_FREQ_12000 |
			AAC_SAMPLING_FREQ_16000 |
			AAC_SAMPLING_FREQ_22050 |
			AAC_SAMPLING_FREQ_24000 |
			AAC_SAMPLING_FREQ_32000 |
			AAC_SAMPLING_FREQ_44100 |
			AAC_SAMPLING_FREQ_48000 |
			AAC_SAMPLING_FREQ_64000 |
			AAC_SAMPLING_FREQ_88200 |
			AAC_SAMPLING_FREQ_96000)
	.channels =
		AAC_CHANNELS_1 |
		AAC_CHANNELS_2,
	.vbr = 1,
	AAC_INIT_BITRATE(320000)
};

static const struct bluez_a2dp_channel_mode a2dp_aac_channels[] = {
	{ BLUEZ_A2DP_CHM_MONO, AAC_CHANNELS_1 },
	{ BLUEZ_A2DP_CHM_STEREO, AAC_CHANNELS_2 },
};

static const struct bluez_a2dp_sampling_freq a2dp_aac_samplings[] = {
	{ 8000, AAC_SAMPLING_FREQ_8000 },
	{ 11025, AAC_SAMPLING_FREQ_11025 },
	{ 12000, AAC_SAMPLING_FREQ_12000 },
	{ 16000, AAC_SAMPLING_FREQ_16000 },
	{ 22050, AAC_SAMPLING_FREQ_22050 },
	{ 24000, AAC_SAMPLING_FREQ_24000 },
	{ 32000, AAC_SAMPLING_FREQ_32000 },
	{ 44100, AAC_SAMPLING_FREQ_44100 },
	{ 48000, AAC_SAMPLING_FREQ_48000 },
	{ 64000, AAC_SAMPLING_FREQ_64000 },
	{ 88200, AAC_SAMPLING_FREQ_88200 },
	{ 96000, AAC_SAMPLING_FREQ_96000 },
};

static const a2dp_aptx_t a2dp_aptx = {
	.info.vendor_id = APTX_VENDOR_ID,
	.info.codec_id = APTX_CODEC_ID,
	.channel_mode =
		/* NOTE: Used apt-X library does not support
		 *       single channel (mono) mode. */
		APTX_CHANNEL_MODE_STEREO,
	.frequency =
		APTX_SAMPLING_FREQ_16000 |
		APTX_SAMPLING_FREQ_32000 |
		APTX_SAMPLING_FREQ_44100 |
		APTX_SAMPLING_FREQ_48000,
};

static const struct bluez_a2dp_channel_mode a2dp_aptx_channels[] = {
	{ BLUEZ_A2DP_CHM_STEREO, APTX_CHANNEL_MODE_STEREO },
};

static const struct bluez_a2dp_sampling_freq a2dp_aptx_samplings[] = {
	{ 16000, APTX_SAMPLING_FREQ_16000 },
	{ 32000, APTX_SAMPLING_FREQ_32000 },
	{ 44100, APTX_SAMPLING_FREQ_44100 },
	{ 48000, APTX_SAMPLING_FREQ_48000 },
};

static const a2dp_ldac_t a2dp_ldac = {
	.info.vendor_id = LDAC_VENDOR_ID,
	.info.codec_id = LDAC_CODEC_ID,
	.channel_mode =
		LDAC_CHANNEL_MODE_MONO |
		LDAC_CHANNEL_MODE_DUAL_CHANNEL |
		LDAC_CHANNEL_MODE_STEREO,
	.frequency =
		/* NOTE: Used LDAC library does not support
		 *       frequencies higher than 96 kHz. */
		LDAC_SAMPLING_FREQ_44100 |
		LDAC_SAMPLING_FREQ_48000 |
		LDAC_SAMPLING_FREQ_88200 |
		LDAC_SAMPLING_FREQ_96000,
};

static const struct bluez_a2dp_channel_mode a2dp_ldac_channels[] = {
	{ BLUEZ_A2DP_CHM_MONO, LDAC_CHANNEL_MODE_MONO },
	{ BLUEZ_A2DP_CHM_DUAL_CHANNEL, LDAC_CHANNEL_MODE_DUAL_CHANNEL },
	{ BLUEZ_A2DP_CHM_STEREO, LDAC_CHANNEL_MODE_STEREO },
};

static const struct bluez_a2dp_sampling_freq a2dp_ldac_samplings[] = {
	{ 44100, LDAC_SAMPLING_FREQ_44100 },
	{ 48000, LDAC_SAMPLING_FREQ_48000 },
	{ 88200, LDAC_SAMPLING_FREQ_88200 },
	{ 96000, LDAC_SAMPLING_FREQ_96000 },
};

static const struct bluez_a2dp_codec a2dp_codec_source_sbc = {
	.dir = BLUEZ_A2DP_SOURCE,
	.id = A2DP_CODEC_SBC,
	.cfg = &a2dp_sbc,
	.cfg_size = sizeof(a2dp_sbc),
	.channels = a2dp_sbc_channels,
	.channels_size = ARRAYSIZE(a2dp_sbc_channels),
	.samplings = a2dp_sbc_samplings,
	.samplings_size = ARRAYSIZE(a2dp_sbc_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_sink_sbc = {
	.dir = BLUEZ_A2DP_SINK,
	.id = A2DP_CODEC_SBC,
	.cfg = &a2dp_sbc,
	.cfg_size = sizeof(a2dp_sbc),
	.channels = a2dp_sbc_channels,
	.channels_size = ARRAYSIZE(a2dp_sbc_channels),
	.samplings = a2dp_sbc_samplings,
	.samplings_size = ARRAYSIZE(a2dp_sbc_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_source_mpeg = {
	.dir = BLUEZ_A2DP_SOURCE,
	.id = A2DP_CODEC_MPEG12,
	.cfg = &a2dp_mpeg,
	.cfg_size = sizeof(a2dp_mpeg),
	.channels = a2dp_mpeg_channels,
	.channels_size = ARRAYSIZE(a2dp_mpeg_channels),
	.samplings = a2dp_mpeg_samplings,
	.samplings_size = ARRAYSIZE(a2dp_mpeg_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_sink_mpeg = {
	.dir = BLUEZ_A2DP_SINK,
	.id = A2DP_CODEC_MPEG12,
	.cfg = &a2dp_mpeg,
	.cfg_size = sizeof(a2dp_mpeg),
	.channels = a2dp_mpeg_channels,
	.channels_size = ARRAYSIZE(a2dp_mpeg_channels),
	.samplings = a2dp_mpeg_samplings,
	.samplings_size = ARRAYSIZE(a2dp_mpeg_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_source_aac = {
	.dir = BLUEZ_A2DP_SOURCE,
	.id = A2DP_CODEC_MPEG24,
	.cfg = &a2dp_aac,
	.cfg_size = sizeof(a2dp_aac),
	.channels = a2dp_aac_channels,
	.channels_size = ARRAYSIZE(a2dp_aac_channels),
	.samplings = a2dp_aac_samplings,
	.samplings_size = ARRAYSIZE(a2dp_aac_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_sink_aac = {
	.dir = BLUEZ_A2DP_SINK,
	.id = A2DP_CODEC_MPEG24,
	.cfg = &a2dp_aac,
	.cfg_size = sizeof(a2dp_aac),
	.channels = a2dp_aac_channels,
	.channels_size = ARRAYSIZE(a2dp_aac_channels),
	.samplings = a2dp_aac_samplings,
	.samplings_size = ARRAYSIZE(a2dp_aac_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_source_aptx = {
	.dir = BLUEZ_A2DP_SOURCE,
	.id = A2DP_CODEC_VENDOR_APTX,
	.cfg = &a2dp_aptx,
	.cfg_size = sizeof(a2dp_aptx),
	.channels = a2dp_aptx_channels,
	.channels_size = ARRAYSIZE(a2dp_aptx_channels),
	.samplings = a2dp_aptx_samplings,
	.samplings_size = ARRAYSIZE(a2dp_aptx_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_sink_aptx = {
	.dir = BLUEZ_A2DP_SINK,
	.id = A2DP_CODEC_VENDOR_APTX,
	.cfg = &a2dp_aptx,
	.cfg_size = sizeof(a2dp_aptx),
	.channels = a2dp_aptx_channels,
	.channels_size = ARRAYSIZE(a2dp_aptx_channels),
	.samplings = a2dp_aptx_samplings,
	.samplings_size = ARRAYSIZE(a2dp_aptx_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_source_ldac = {
	.dir = BLUEZ_A2DP_SOURCE,
	.id = A2DP_CODEC_VENDOR_LDAC,
	.cfg = &a2dp_ldac,
	.cfg_size = sizeof(a2dp_ldac),
	.channels = a2dp_ldac_channels,
	.channels_size = ARRAYSIZE(a2dp_ldac_channels),
	.samplings = a2dp_ldac_samplings,
	.samplings_size = ARRAYSIZE(a2dp_ldac_samplings),
};

static const struct bluez_a2dp_codec a2dp_codec_sink_ldac = {
	.dir = BLUEZ_A2DP_SINK,
	.id = A2DP_CODEC_VENDOR_LDAC,
	.cfg = &a2dp_ldac,
	.cfg_size = sizeof(a2dp_ldac),
	.channels = a2dp_ldac_channels,
	.channels_size = ARRAYSIZE(a2dp_ldac_channels),
	.samplings = a2dp_ldac_samplings,
	.samplings_size = ARRAYSIZE(a2dp_ldac_samplings),
};

static const struct bluez_a2dp_codec *a2dp_codecs[] = {
#if ENABLE_LDAC
	&a2dp_codec_source_ldac,
#endif
#if ENABLE_APTX
	&a2dp_codec_source_aptx,
#endif
#if ENABLE_AAC
	&a2dp_codec_source_aac,
	&a2dp_codec_sink_aac,
#endif
#if ENABLE_MPEG
	&a2dp_codec_source_mpeg,
	&a2dp_codec_sink_mpeg,
#endif
	&a2dp_codec_source_sbc,
	&a2dp_codec_sink_sbc,
	NULL,
};

const struct bluez_a2dp_codec **bluez_a2dp_codecs = a2dp_codecs;
