/*
 * BlueALSA - a2dp.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp.h"

#include <errno.h>
#include <stdbool.h>

#include "a2dp-codecs.h"
#include "hci.h"
#include "shared/defs.h"
#include "shared/log.h"

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

static const struct a2dp_channel_mode a2dp_sbc_channels[] = {
	{ A2DP_CHM_MONO, 1, SBC_CHANNEL_MODE_MONO },
	{ A2DP_CHM_DUAL_CHANNEL, 2, SBC_CHANNEL_MODE_DUAL_CHANNEL },
	{ A2DP_CHM_STEREO, 2, SBC_CHANNEL_MODE_STEREO },
	{ A2DP_CHM_JOINT_STEREO, 2, SBC_CHANNEL_MODE_JOINT_STEREO },
};

static const struct a2dp_sampling_freq a2dp_sbc_samplings[] = {
	{ 16000, SBC_SAMPLING_FREQ_16000 },
	{ 32000, SBC_SAMPLING_FREQ_32000 },
	{ 44100, SBC_SAMPLING_FREQ_44100 },
	{ 48000, SBC_SAMPLING_FREQ_48000 },
};

static const a2dp_mpeg_t a2dp_mpeg_source = {
	.layer =
		MPEG_LAYER_MP3,
	.crc = 1,
	.channel_mode =
	/* NOTE: LAME does not support dual-channel mode. */
		MPEG_CHANNEL_MODE_MONO |
		MPEG_CHANNEL_MODE_STEREO |
		MPEG_CHANNEL_MODE_JOINT_STEREO,
	/* NOTE: Since MPF-2 is not required for neither Sink
	 *       nor Source, we are not going to support it. */
	.mpf = 0,
	.frequency =
		MPEG_SAMPLING_FREQ_16000 |
		MPEG_SAMPLING_FREQ_22050 |
		MPEG_SAMPLING_FREQ_24000 |
		MPEG_SAMPLING_FREQ_32000 |
		MPEG_SAMPLING_FREQ_44100 |
		MPEG_SAMPLING_FREQ_48000,
	.vbr = 1,
	MPEG_INIT_BITRATE(
		MPEG_BIT_RATE_INDEX_0 |
		MPEG_BIT_RATE_INDEX_1 |
		MPEG_BIT_RATE_INDEX_2 |
		MPEG_BIT_RATE_INDEX_3 |
		MPEG_BIT_RATE_INDEX_4 |
		MPEG_BIT_RATE_INDEX_5 |
		MPEG_BIT_RATE_INDEX_6 |
		MPEG_BIT_RATE_INDEX_7 |
		MPEG_BIT_RATE_INDEX_8 |
		MPEG_BIT_RATE_INDEX_9 |
		MPEG_BIT_RATE_INDEX_10 |
		MPEG_BIT_RATE_INDEX_11 |
		MPEG_BIT_RATE_INDEX_12 |
		MPEG_BIT_RATE_INDEX_13 |
		MPEG_BIT_RATE_INDEX_14
	)
};

static const a2dp_mpeg_t a2dp_mpeg_sink = {
	.layer =
#if ENABLE_MPG123
		MPEG_LAYER_MP1 |
		MPEG_LAYER_MP2 |
#endif
		MPEG_LAYER_MP3,
	.crc = 1,
	.channel_mode =
	/* NOTE: LAME does not support dual-channel mode. Be aware, that
	 *       lack of this feature violates A2DP Sink specification. */
		MPEG_CHANNEL_MODE_MONO |
#if ENABLE_MPG123
		MPEG_CHANNEL_MODE_DUAL_CHANNEL |
#endif
		MPEG_CHANNEL_MODE_STEREO |
		MPEG_CHANNEL_MODE_JOINT_STEREO,
	/* NOTE: Since MPF-2 is not required for neither Sink
	 *       nor Source, we are not going to support it. */
	.mpf = 0,
	.frequency =
		MPEG_SAMPLING_FREQ_16000 |
		MPEG_SAMPLING_FREQ_22050 |
		MPEG_SAMPLING_FREQ_24000 |
		MPEG_SAMPLING_FREQ_32000 |
		MPEG_SAMPLING_FREQ_44100 |
		MPEG_SAMPLING_FREQ_48000,
	.vbr = 1,
	MPEG_INIT_BITRATE(
		MPEG_BIT_RATE_INDEX_0 |
		MPEG_BIT_RATE_INDEX_1 |
		MPEG_BIT_RATE_INDEX_2 |
		MPEG_BIT_RATE_INDEX_3 |
		MPEG_BIT_RATE_INDEX_4 |
		MPEG_BIT_RATE_INDEX_5 |
		MPEG_BIT_RATE_INDEX_6 |
		MPEG_BIT_RATE_INDEX_7 |
		MPEG_BIT_RATE_INDEX_8 |
		MPEG_BIT_RATE_INDEX_9 |
		MPEG_BIT_RATE_INDEX_10 |
		MPEG_BIT_RATE_INDEX_11 |
		MPEG_BIT_RATE_INDEX_12 |
		MPEG_BIT_RATE_INDEX_13 |
		MPEG_BIT_RATE_INDEX_14
	)
};

static const struct a2dp_channel_mode a2dp_mpeg_channels[] = {
	{ A2DP_CHM_MONO, 1, MPEG_CHANNEL_MODE_MONO },
	{ A2DP_CHM_DUAL_CHANNEL, 2, MPEG_CHANNEL_MODE_DUAL_CHANNEL },
	{ A2DP_CHM_STEREO, 2, MPEG_CHANNEL_MODE_STEREO },
	{ A2DP_CHM_JOINT_STEREO, 2, MPEG_CHANNEL_MODE_JOINT_STEREO },
};

static const struct a2dp_sampling_freq a2dp_mpeg_samplings[] = {
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

static const struct a2dp_channel_mode a2dp_aac_channels[] = {
	{ A2DP_CHM_MONO, 1, AAC_CHANNELS_1 },
	{ A2DP_CHM_STEREO, 2, AAC_CHANNELS_2 },
};

static const struct a2dp_sampling_freq a2dp_aac_samplings[] = {
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
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
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

static const struct a2dp_channel_mode a2dp_aptx_channels[] = {
	{ A2DP_CHM_STEREO, 2, APTX_CHANNEL_MODE_STEREO },
};

static const struct a2dp_sampling_freq a2dp_aptx_samplings[] = {
	{ 16000, APTX_SAMPLING_FREQ_16000 },
	{ 32000, APTX_SAMPLING_FREQ_32000 },
	{ 44100, APTX_SAMPLING_FREQ_44100 },
	{ 48000, APTX_SAMPLING_FREQ_48000 },
};

static const a2dp_faststream_t a2dp_faststream = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
	.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE,
	.frequency_music =
		FASTSTREAM_SAMPLING_FREQ_MUSIC_44100 |
		FASTSTREAM_SAMPLING_FREQ_MUSIC_48000,
	.frequency_voice =
		FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
};

static const struct a2dp_sampling_freq a2dp_faststream_samplings_music[] = {
	{ 44100, FASTSTREAM_SAMPLING_FREQ_MUSIC_44100 },
	{ 48000, FASTSTREAM_SAMPLING_FREQ_MUSIC_48000 },
};

static const struct a2dp_sampling_freq a2dp_faststream_samplings_voice[] = {
	{ 16000, FASTSTREAM_SAMPLING_FREQ_VOICE_16000 },
};

static const a2dp_aptx_hd_t a2dp_aptx_hd = {
	.aptx.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
	.aptx.channel_mode =
		/* NOTE: Used apt-X HD library does not support
		 *       single channel (mono) mode. */
		APTX_CHANNEL_MODE_STEREO,
	.aptx.frequency =
		APTX_SAMPLING_FREQ_16000 |
		APTX_SAMPLING_FREQ_32000 |
		APTX_SAMPLING_FREQ_44100 |
		APTX_SAMPLING_FREQ_48000,
};

static const struct a2dp_channel_mode a2dp_aptx_hd_channels[] = {
	{ A2DP_CHM_STEREO, 2, APTX_CHANNEL_MODE_STEREO },
};

static const struct a2dp_sampling_freq a2dp_aptx_hd_samplings[] = {
	{ 16000, APTX_SAMPLING_FREQ_16000 },
	{ 32000, APTX_SAMPLING_FREQ_32000 },
	{ 44100, APTX_SAMPLING_FREQ_44100 },
	{ 48000, APTX_SAMPLING_FREQ_48000 },
};

static const a2dp_ldac_t a2dp_ldac = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID),
	.channel_mode =
		LDAC_CHANNEL_MODE_MONO |
		LDAC_CHANNEL_MODE_DUAL |
		LDAC_CHANNEL_MODE_STEREO,
	.frequency =
		/* NOTE: Used LDAC library does not support
		 *       frequencies higher than 96 kHz. */
		LDAC_SAMPLING_FREQ_44100 |
		LDAC_SAMPLING_FREQ_48000 |
		LDAC_SAMPLING_FREQ_88200 |
		LDAC_SAMPLING_FREQ_96000,
};

static const struct a2dp_channel_mode a2dp_ldac_channels[] = {
	{ A2DP_CHM_MONO, 1, LDAC_CHANNEL_MODE_MONO },
	{ A2DP_CHM_DUAL_CHANNEL, 2, LDAC_CHANNEL_MODE_DUAL },
	{ A2DP_CHM_STEREO, 2, LDAC_CHANNEL_MODE_STEREO },
};

static const struct a2dp_sampling_freq a2dp_ldac_samplings[] = {
	{ 44100, LDAC_SAMPLING_FREQ_44100 },
	{ 48000, LDAC_SAMPLING_FREQ_48000 },
	{ 88200, LDAC_SAMPLING_FREQ_88200 },
	{ 96000, LDAC_SAMPLING_FREQ_96000 },
};

static const struct a2dp_codec a2dp_codec_source_sbc = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_SBC,
	.capabilities = &a2dp_sbc,
	.capabilities_size = sizeof(a2dp_sbc),
	.channels[0] = a2dp_sbc_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_sbc_channels),
	.samplings[0] = a2dp_sbc_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_sbc_samplings),
};

static const struct a2dp_codec a2dp_codec_sink_sbc = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_SBC,
	.capabilities = &a2dp_sbc,
	.capabilities_size = sizeof(a2dp_sbc),
	.channels[0] = a2dp_sbc_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_sbc_channels),
	.samplings[0] = a2dp_sbc_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_sbc_samplings),
};

static const struct a2dp_codec a2dp_codec_source_mpeg = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_MPEG12,
	.capabilities = &a2dp_mpeg_source,
	.capabilities_size = sizeof(a2dp_mpeg_source),
	.channels[0] = a2dp_mpeg_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_mpeg_channels),
	.samplings[0] = a2dp_mpeg_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_mpeg_samplings),
};

static const struct a2dp_codec a2dp_codec_sink_mpeg = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_MPEG12,
	.capabilities = &a2dp_mpeg_sink,
	.capabilities_size = sizeof(a2dp_mpeg_sink),
	.channels[0] = a2dp_mpeg_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_mpeg_channels),
	.samplings[0] = a2dp_mpeg_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_mpeg_samplings),
};

static const struct a2dp_codec a2dp_codec_source_aac = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_MPEG24,
	.capabilities = &a2dp_aac,
	.capabilities_size = sizeof(a2dp_aac),
	.channels[0] = a2dp_aac_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aac_channels),
	.samplings[0] = a2dp_aac_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aac_samplings),
};

static const struct a2dp_codec a2dp_codec_sink_aac = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_MPEG24,
	.capabilities = &a2dp_aac,
	.capabilities_size = sizeof(a2dp_aac),
	.channels[0] = a2dp_aac_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aac_channels),
	.samplings[0] = a2dp_aac_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aac_samplings),
};

static const struct a2dp_codec a2dp_codec_source_aptx = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_APTX,
	.capabilities = &a2dp_aptx,
	.capabilities_size = sizeof(a2dp_aptx),
	.channels[0] = a2dp_aptx_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aptx_channels),
	.samplings[0] = a2dp_aptx_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aptx_samplings),
};

static const struct a2dp_codec a2dp_codec_sink_aptx = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_APTX,
	.capabilities = &a2dp_aptx,
	.capabilities_size = sizeof(a2dp_aptx),
	.channels[0] = a2dp_aptx_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aptx_channels),
	.samplings[0] = a2dp_aptx_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aptx_samplings),
};

static const struct a2dp_codec a2dp_codec_source_faststream = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_FASTSTREAM,
	.backchannel = true,
	.capabilities = &a2dp_faststream,
	.capabilities_size = sizeof(a2dp_faststream),
	.samplings[0] = a2dp_faststream_samplings_music,
	.samplings_size[0] = ARRAYSIZE(a2dp_faststream_samplings_music),
	.samplings[1] = a2dp_faststream_samplings_voice,
	.samplings_size[1] = ARRAYSIZE(a2dp_faststream_samplings_voice),
};

static const struct a2dp_codec a2dp_codec_sink_faststream = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_FASTSTREAM,
	.backchannel = true,
	.capabilities = &a2dp_faststream,
	.capabilities_size = sizeof(a2dp_faststream),
	.samplings[0] = a2dp_faststream_samplings_music,
	.samplings_size[0] = ARRAYSIZE(a2dp_faststream_samplings_music),
	.samplings[1] = a2dp_faststream_samplings_voice,
	.samplings_size[1] = ARRAYSIZE(a2dp_faststream_samplings_voice),
};

static const struct a2dp_codec a2dp_codec_source_aptx_hd = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_APTX_HD,
	.capabilities = &a2dp_aptx_hd,
	.capabilities_size = sizeof(a2dp_aptx_hd),
	.channels[0] = a2dp_aptx_hd_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aptx_hd_channels),
	.samplings[0] = a2dp_aptx_hd_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aptx_hd_samplings),
};

static const struct a2dp_codec a2dp_codec_sink_aptx_hd = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_APTX_HD,
	.capabilities = &a2dp_aptx_hd,
	.capabilities_size = sizeof(a2dp_aptx_hd),
	.channels[0] = a2dp_aptx_hd_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aptx_hd_channels),
	.samplings[0] = a2dp_aptx_hd_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aptx_hd_samplings),
};

static const struct a2dp_codec a2dp_codec_source_ldac = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_LDAC,
	.capabilities = &a2dp_ldac,
	.capabilities_size = sizeof(a2dp_ldac),
	.channels[0] = a2dp_ldac_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_ldac_channels),
	.samplings[0] = a2dp_ldac_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_ldac_samplings),
};

static const struct a2dp_codec a2dp_codec_sink_ldac = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_LDAC,
	.capabilities = &a2dp_ldac,
	.capabilities_size = sizeof(a2dp_ldac),
	.channels[0] = a2dp_ldac_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_ldac_channels),
	.samplings[0] = a2dp_ldac_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_ldac_samplings),
};

const struct a2dp_codec *a2dp_codecs[] = {
#if ENABLE_LDAC
	&a2dp_codec_source_ldac,
#endif
#if ENABLE_APTX_HD
	&a2dp_codec_source_aptx_hd,
#endif
#if ENABLE_FASTSTREAM
	&a2dp_codec_source_faststream,
	&a2dp_codec_sink_faststream,
#endif
#if ENABLE_APTX
	&a2dp_codec_source_aptx,
#endif
#if ENABLE_AAC
	&a2dp_codec_source_aac,
	&a2dp_codec_sink_aac,
#endif
#if ENABLE_MPEG
# if ENABLE_MP3LAME
	&a2dp_codec_source_mpeg,
# endif
# if ENABLE_MP3LAME || ENABLE_MPG123
	&a2dp_codec_sink_mpeg,
# endif
#endif
	&a2dp_codec_source_sbc,
	&a2dp_codec_sink_sbc,
	NULL,
};

/**
 * Get A2DP 16-bit vendor codec ID - BlueALSA extension.
 *
 * @param capabilities A2DP vendor codec capabilities.
 * @param size A2DP vendor codec capabilities size.
 * @return On success this function returns A2DP 16-bit vendor codec ID. */
uint16_t a2dp_get_vendor_codec_id(const void *capabilities, size_t size) {

	if (size < sizeof(a2dp_vendor_codec_t))
		return errno = EINVAL, 0xFFFF;

	uint32_t vendor_id = A2DP_GET_VENDOR_ID(*(a2dp_vendor_codec_t *)capabilities);
	uint16_t codec_id = A2DP_GET_CODEC_ID(*(a2dp_vendor_codec_t *)capabilities);

	switch (vendor_id) {
	case BT_COMPID_QUALCOMM_TECH_INTL:
		switch (codec_id) {
		case FASTSTREAM_CODEC_ID:
			return A2DP_CODEC_VENDOR_FASTSTREAM;
		case APTX_LL_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX_LL;
		} break;
	case BT_COMPID_APPLE:
		switch (codec_id) {
		} break;
	case BT_COMPID_APT:
		switch (codec_id) {
		case APTX_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX;
		} break;
	case BT_COMPID_SAMSUNG_ELEC:
		switch (codec_id) {
		case SAMSUNG_HD_CODEC_ID:
			return A2DP_CODEC_VENDOR_SAMSUNG_HD;
		case SAMSUNG_SC_CODEC_ID:
			return A2DP_CODEC_VENDOR_SAMSUNG_SC;
		} break;
	case BT_COMPID_QUALCOMM_TECH:
		switch (codec_id) {
		case APTX_HD_CODEC_ID:
			return A2DP_CODEC_VENDOR_APTX_HD;
		} break;
	case BT_COMPID_SONY:
		switch (codec_id) {
		case LDAC_CODEC_ID:
			return A2DP_CODEC_VENDOR_LDAC;
		} break;
	case BT_COMPID_SAVITECH:
		switch (codec_id) {
		case LHDC_CODEC_ID:
			return A2DP_CODEC_VENDOR_LHDC;
		case LHDC_V1_CODEC_ID:
			return A2DP_CODEC_VENDOR_LHDC_V1;
		case LLAC_CODEC_ID:
			return A2DP_CODEC_VENDOR_LLAC;
		} break;
	}

	hexdump("Unknown vendor codec", capabilities, size);

	errno = ENOTSUP;
	return 0xFFFF;
}
