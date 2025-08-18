/*
 * BlueALSA - a2dp-aac.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-aac.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/aacenc_lib.h>
#define AACENCODER_LIB_VERSION LIB_VERSION( \
		AACENCODER_LIB_VL0, AACENCODER_LIB_VL1, AACENCODER_LIB_VL2)

#include <glib.h>

#include "a2dp.h"
#include "ba-config.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_bit_mapping a2dp_aac_channels[] = {
	{ AAC_CHANNEL_MODE_MONO, .ch = { 1, a2dp_channel_map_mono } },
	{ AAC_CHANNEL_MODE_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ AAC_CHANNEL_MODE_5_1, .ch = { 6, a2dp_channel_map_5_1 } },
	{ AAC_CHANNEL_MODE_7_1, .ch = { 8, a2dp_channel_map_7_1 } },
	{ 0 }
};

static const struct a2dp_bit_mapping a2dp_aac_rates[] = {
	{ AAC_SAMPLING_FREQ_8000, { 8000 } },
	{ AAC_SAMPLING_FREQ_11025, { 11025 } },
	{ AAC_SAMPLING_FREQ_12000, { 12000 } },
	{ AAC_SAMPLING_FREQ_16000, { 16000 } },
	{ AAC_SAMPLING_FREQ_22050, { 22050 } },
	{ AAC_SAMPLING_FREQ_24000, { 24000 } },
	{ AAC_SAMPLING_FREQ_32000, { 32000 } },
	{ AAC_SAMPLING_FREQ_44100, { 44100 } },
	{ AAC_SAMPLING_FREQ_48000, { 48000 } },
	{ AAC_SAMPLING_FREQ_64000, { 64000 } },
	{ AAC_SAMPLING_FREQ_88200, { 88200 } },
	{ AAC_SAMPLING_FREQ_96000, { 96000 } },
	{ 0 }
};

static void a2dp_aac_caps_intersect(
		void *capabilities,
		const void *mask) {

	const a2dp_aac_t *caps_mask = mask;
	a2dp_aac_t *caps = capabilities;

	int rate = MIN(A2DP_AAC_GET_BITRATE(*caps), A2DP_AAC_GET_BITRATE(*caps_mask));

	a2dp_caps_bitwise_intersect(caps, caps_mask, sizeof(*caps));
	A2DP_AAC_SET_BITRATE(*caps, rate);

}

static int a2dp_aac_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_aac_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_aac_channels, caps->channel_mode, func, userdata);
	return -1;
}

static int a2dp_aac_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_aac_t *caps = capabilities;
	if (stream == A2DP_MAIN) {
		const uint16_t sampling_freq = A2DP_AAC_GET_SAMPLING_FREQ(*caps);
		return a2dp_bit_mapping_foreach(a2dp_aac_rates, sampling_freq, func, userdata);
	}
	return -1;
}

static void a2dp_aac_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	a2dp_aac_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->channel_mode = a2dp_bit_mapping_lookup_value(a2dp_aac_channels,
				caps->channel_mode, channels);
}

static void a2dp_aac_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_aac_t *caps = capabilities;
	if (stream == A2DP_MAIN) {
		const uint16_t sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_aac_rates,
				A2DP_AAC_GET_SAMPLING_FREQ(*caps), rate);
		A2DP_AAC_SET_SAMPLING_FREQ(*caps, sampling_freq);
	}
}

static struct a2dp_caps_helpers a2dp_aac_caps_helpers = {
	.intersect = a2dp_aac_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_aac_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_aac_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_aac_caps_select_channel_mode,
	.select_sample_rate = a2dp_aac_caps_select_sample_rate,
};

static unsigned int a2dp_aac_get_fdk_vbr_mode(
		unsigned int channels, unsigned int bitrate) {
	static const unsigned int modes[][5] = {
		/* bitrate upper bounds for mono channel mode */
		{ 32000, 40000, 56000, 72000, 112000 },
		/* bitrate upper bounds for stereo channel mode */
		{ 40000, 64000, 96000, 128000, 192000 },
	};
	const size_t ch = channels == 1 ? 0 : 1;
	for (size_t i = 0; i < ARRAYSIZE(modes[ch]); i++)
		if (bitrate <= modes[ch][i])
			return i + 1;
	return 5;
}

void *a2dp_aac_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_AACENCODER handle;
	AACENC_InfoStruct info;
	AACENC_ERROR err;

	const a2dp_aac_t *configuration = &t->media.configuration.aac;
	const unsigned int bitrate = A2DP_AAC_GET_BITRATE(*configuration);
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	/* create AAC encoder without the Meta Data module */
	if ((err = aacEncOpen(&handle, 0x0F, channels)) != AACENC_OK) {
		error("Couldn't open AAC encoder: %s", aacenc_strerror(err));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(aacEncClose), &handle);

	unsigned int aot = AOT_NONE;
	switch (configuration->object_type) {
	case AAC_OBJECT_TYPE_MPEG2_LC:
#if AACENCODER_LIB_VERSION <= 0x03040C00 /* 3.4.12 */ || \
		AACENCODER_LIB_VERSION >= 0x04000000 /* 4.0.0 */
		aot = AOT_MP2_AAC_LC;
		break;
#endif
	case AAC_OBJECT_TYPE_MPEG4_LC:
		aot = AOT_AAC_LC;
		break;
	case AAC_OBJECT_TYPE_MPEG4_LTP:
		aot = AOT_AAC_LTP;
		break;
	case AAC_OBJECT_TYPE_MPEG4_SCA:
		aot = AOT_AAC_SCAL;
		break;
	case AAC_OBJECT_TYPE_MPEG4_HE:
		aot = AOT_SBR;
		break;
	case AAC_OBJECT_TYPE_MPEG4_HE2:
		aot = AOT_PS;
		break;
	case AAC_OBJECT_TYPE_MPEG4_ELD2:
		aot = AOT_ER_AAC_ELD;
		break;
	}

	unsigned int channel_mode = MODE_1;
	switch (configuration->channel_mode) {
	case AAC_CHANNEL_MODE_MONO:
		channel_mode = MODE_1;
		break;
	case AAC_CHANNEL_MODE_STEREO:
		channel_mode = MODE_2;
		break;
	case AAC_CHANNEL_MODE_5_1:
		channel_mode = MODE_1_2_2_1;
		break;
	case AAC_CHANNEL_MODE_7_1:
		channel_mode = MODE_1_2_2_2_1;
		break;
	}

	if ((err = aacEncoder_SetParam(handle, AACENC_AOT, aot)) != AACENC_OK) {
		error("Couldn't set audio object type: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_BITRATE, bitrate)) != AACENC_OK) {
		error("Couldn't set bitrate: %s", aacenc_strerror(err));
		goto fail_init;
	}
#if AACENCODER_LIB_VERSION >= 0x03041600 /* 3.4.22 */
	if (!config.aac_true_bps) {
		if ((err = aacEncoder_SetParam(handle, AACENC_PEAK_BITRATE, bitrate)) != AACENC_OK) {
			error("Couldn't set peak bitrate: %s", aacenc_strerror(err));
			goto fail_init;
		}
	}
#endif
	if ((err = aacEncoder_SetParam(handle, AACENC_SAMPLERATE, rate)) != AACENC_OK) {
		error("Couldn't set sample rate: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_CHANNELMODE, channel_mode)) != AACENC_OK) {
		error("Couldn't set channel mode: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if (configuration->vbr) {
		const unsigned int mode = a2dp_aac_get_fdk_vbr_mode(channels, bitrate);
		if ((err = aacEncoder_SetParam(handle, AACENC_BITRATEMODE, mode)) != AACENC_OK) {
			error("Couldn't set VBR bitrate mode %u: %s", mode, aacenc_strerror(err));
			goto fail_init;
		}
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_AFTERBURNER, config.aac_afterburner)) != AACENC_OK) {
		error("Couldn't enable afterburner: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_TRANSMUX, TT_MP4_LATM_MCP1)) != AACENC_OK) {
		error("Couldn't enable LATM transport type: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_HEADER_PERIOD, 1)) != AACENC_OK) {
		error("Couldn't set LATM header period: %s", aacenc_strerror(err));
		goto fail_init;
	}
#if AACENCODER_LIB_VERSION >= 0x03041600 /* 3.4.22 */
	if ((err = aacEncoder_SetParam(handle, AACENC_AUDIOMUXVER, config.aac_latm_version)) != AACENC_OK) {
		error("Couldn't set LATM version: %s", aacenc_strerror(err));
		goto fail_init;
	}
#endif

	if ((err = aacEncEncode(handle, NULL, NULL, NULL, NULL)) != AACENC_OK) {
		error("Couldn't initialize AAC encoder: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncInfo(handle, &info)) != AACENC_OK) {
		error("Couldn't get encoder info: %s", aacenc_strerror(err));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const size_t aac_frame_pcm_samples = info.inputChannels * info.frameLength;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	if (ffb_init(&pcm, aac_frame_pcm_samples, sample_size) == -1 ||
			ffb_init_uint8_t(&bt, RTP_HEADER_LEN + info.maxOutBufBytes) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	/* Get the delay introduced by the encoder. */
	t_pcm->codec_delay_dms = info.nDelay * 10000 / rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

	rtp_header_t *rtp_header;
	/* initialize RTP header and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header, NULL, 0);

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to 90kHz */
	rtp_state_init(&rtp, rate, 90000);

	int in_bufferIdentifiers[] = { IN_AUDIO_DATA };
	int out_bufferIdentifiers[] = { OUT_BITSTREAM_DATA };
	int in_bufSizes[] = { pcm.nmemb * pcm.size };
	int out_bufSizes[] = { info.maxOutBufBytes };
	int in_bufElSizes[] = { pcm.size };
	int out_bufElSizes[] = { bt.size };

	AACENC_BufDesc in_buf = {
		.numBufs = 1,
		.bufs = (void **)&pcm.data,
		.bufferIdentifiers = in_bufferIdentifiers,
		.bufSizes = in_bufSizes,
		.bufElSizes = in_bufElSizes,
	};
	AACENC_BufDesc out_buf = {
		.numBufs = 1,
		.bufs = (void **)&rtp_payload,
		.bufferIdentifiers = out_bufferIdentifiers,
		.bufSizes = out_bufSizes,
		.bufElSizes = out_bufElSizes,
	};
	AACENC_InArgs in_args = { 0 };
	AACENC_OutArgs out_args = { 0 };

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				in_args.numInSamples = -1;
				/* flush encoder internal buffers */
				while (aacEncEncode(handle, NULL, &out_buf, &in_args, &out_args) == AACENC_OK)
					continue;
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		while ((in_args.numInSamples = ffb_len_out(&pcm)) > (int)info.inputChannels) {

			if ((err = aacEncEncode(handle, &in_buf, &out_buf, &in_args, &out_args)) != AACENC_OK)
				error("AAC encoding error: %s", aacenc_strerror(err));

			if (out_args.numOutBytes > 0) {

				size_t payload_len_max = t->mtu_write - RTP_HEADER_LEN;
				size_t payload_len = out_args.numOutBytes;

				/* If the size of the RTP packet exceeds writing MTU, the RTP payload
				 * should be fragmented. According to the RFC 3016, fragmentation of
				 * the audioMuxElement requires no extra header - the payload should
				 * be fragmented and spread across multiple RTP packets. */
				for (;;) {

					size_t chunk_len;
					chunk_len = payload_len > payload_len_max ? payload_len_max : payload_len;
					rtp_header->markbit = payload_len <= payload_len_max;
					rtp_state_new_frame(&rtp, rtp_header);

					ffb_rewind(&bt);
					ffb_seek(&bt, RTP_HEADER_LEN + chunk_len);

					ssize_t len = ffb_blen_out(&bt);
					if ((len = io_bt_write(t_pcm, bt.data, len)) <= 0) {
						if (len == -1)
							error("BT write error: %s", strerror(errno));
						goto fail;
					}

					if (!io.initiated) {
						/* Get the delay due to codec processing. */
						t_pcm->processing_delay_dms = asrsync_get_dms_since_last_sync(&io.asrs);
						ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);
						io.initiated = true;
					}

					/* resend RTP header */
					len -= RTP_HEADER_LEN;

					/* break if there is no more payload data */
					if ((payload_len -= len) == 0)
						break;

					/* move the rest of data to the beginning of payload */
					debug("AAC payload fragmentation: extra %zu bytes", payload_len);
					memmove(rtp_payload, rtp_payload + len, payload_len);

				}

			}

			const size_t pcm_frames = out_args.numInSamples / info.inputChannels;
			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, pcm_frames);

			/* If the input buffer was not consumed, we have to append new data to
			 * the existing one. Since we do not use ring buffer, we will simply
			 * move unprocessed data to the front of our linear buffer. */
			ffb_shift(&pcm, out_args.numInSamples);

		}

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}

__attribute__ ((weak))
void *a2dp_aac_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_AACDECODER handle;
	AAC_DECODER_ERROR err;

	if ((handle = aacDecoder_Open(TT_MP4_LATM_MCP1, 1)) == NULL) {
		error("Couldn't open AAC decoder");
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(aacDecoder_Close), handle);

	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

#ifdef AACDECODER_LIB_VL0
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_MIN_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set min output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_MAX_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set max output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
#else
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
#endif

	ffb_t bt = { 0 };
	ffb_t latm = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &latm);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int16_t(&pcm, 2048 * channels) == -1 ||
			ffb_init_uint8_t(&latm, t->mtu_read) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to 90kHz */
	rtp_state_init(&rtp, rate, 90000);

	int markbit_quirk = -3;

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len;
		ffb_rewind(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		const uint8_t *rtp_latm;
		const rtp_header_t *rtp_header = bt.data;
		if ((rtp_latm = rtp_a2dp_get_payload(rtp_header)) == NULL)
			continue;

		int missing_rtp_frames = 0;
		rtp_state_sync_stream(&rtp, rtp_header, &missing_rtp_frames, NULL);

		if (!ba_transport_pcm_is_active(t_pcm)) {
			rtp.synced = false;
			continue;
		}

		size_t rtp_latm_len = len - (rtp_latm - (uint8_t *)bt.data);

		/* If in the first N packets mark bit is not set, it might mean, that
		 * the mark bit will not be set at all. In such a case, activate mark
		 * bit quirk workaround. */
		if (markbit_quirk < 0) {
			if (rtp_header->markbit)
				markbit_quirk = 0;
			else if (++markbit_quirk == 0) {
				warn("Activating RTP mark bit quirk workaround");
				markbit_quirk = 1;
			}
		}

		if (ffb_len_in(&latm) < rtp_latm_len) {
			debug("Resizing LATM buffer: %zd -> %zd", latm.nmemb, latm.nmemb + t->mtu_read);
			if (ffb_init_uint8_t(&latm, latm.nmemb + t->mtu_read) == -1)
				error("Couldn't resize LATM buffer: %s", strerror(errno));
		}

		if (ffb_len_in(&latm) >= rtp_latm_len) {
			memcpy(latm.tail, rtp_latm, rtp_latm_len);
			ffb_seek(&latm, rtp_latm_len);
		}

		if (markbit_quirk != 1 && !rtp_header->markbit) {
			debug("Fragmented RTP packet [%u]: LATM len: %zd", rtp.seq_number, rtp_latm_len);
			continue;
		}

		unsigned int data_len = ffb_len_out(&latm);
		unsigned int valid = ffb_len_out(&latm);
		CStreamInfo *info;

		if ((err = aacDecoder_Fill(handle, (uint8_t **)&latm.data, &data_len, &valid)) != AAC_DEC_OK)
			error("AAC buffer fill error: %s", aacdec_strerror(err));
		else if ((err = aacDecoder_DecodeFrame(handle, pcm.tail, ffb_blen_in(&pcm), 0)) != AAC_DEC_OK)
			error("AAC decode frame error: %s", aacdec_strerror(err));
		else if ((info = aacDecoder_GetStreamInfo(handle)) == NULL)
			error("Couldn't get AAC stream info");
		else {

			if ((unsigned int)info->numChannels != channels)
				warn("AAC channels mismatch: %u != %u", info->numChannels, channels);

			const size_t samples = (size_t)info->frameSize * channels;
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("PCM write error: %s", strerror(errno));

			/* Update the delay introduced by the decoder. */
			t_pcm->codec_delay_dms = info->outputDelay * 10000 / rate;
			ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

			/* update local state with decoded PCM frames */
			rtp_state_update(&rtp, info->frameSize);

		}

		/* make room for new LATM frame */
		ffb_rewind(&latm);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}

static int a2dp_aac_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_aac_t *caps = capabilities;
	const a2dp_aac_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_aac_caps_intersect(caps, &sep->config.capabilities);

	unsigned int channel_mode = 0;
	if (a2dp_aac_caps_foreach_channel_mode(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode) != -1)
		caps->channel_mode = channel_mode;
	else {
		error("AAC: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	unsigned int sampling_freq = 0;
	if (a2dp_aac_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		A2DP_AAC_SET_SAMPLING_FREQ(*caps, sampling_freq);
	else {
		error("AAC: No supported sample rates: %#x", A2DP_AAC_GET_SAMPLING_FREQ(saved));
		return errno = ENOTSUP, -1;
	}

	if (caps->object_type & AAC_OBJECT_TYPE_MPEG4_HE2 &&
			/* The HEv2 uses SBR with Parametric Stereo algorithm
			 * which works only with stereo channel mode. */
			channel_mode == AAC_CHANNEL_MODE_STEREO &&
			/* High-Efficiency AAC Profile requires sampling
			 * frequency of at least 16 kHz. */
			sampling_freq != AAC_SAMPLING_FREQ_8000 &&
			sampling_freq != AAC_SAMPLING_FREQ_11025 &&
			sampling_freq != AAC_SAMPLING_FREQ_12000)
		caps->object_type = AAC_OBJECT_TYPE_MPEG4_HE2;
	else if (caps->object_type & AAC_OBJECT_TYPE_MPEG4_HE &&
			/* High-Efficiency AAC Profile requires sampling
			 * frequency of at least 16 kHz. */
			sampling_freq != AAC_SAMPLING_FREQ_8000 &&
			sampling_freq != AAC_SAMPLING_FREQ_11025 &&
			sampling_freq != AAC_SAMPLING_FREQ_12000)
		caps->object_type = AAC_OBJECT_TYPE_MPEG4_HE;
	else if (caps->object_type & AAC_OBJECT_TYPE_MPEG4_ELD2)
		caps->object_type = AAC_OBJECT_TYPE_MPEG4_ELD2;
	else if (caps->object_type & AAC_OBJECT_TYPE_MPEG4_SCA)
		caps->object_type = AAC_OBJECT_TYPE_MPEG4_SCA;
	else if (caps->object_type & AAC_OBJECT_TYPE_MPEG4_LTP)
		caps->object_type = AAC_OBJECT_TYPE_MPEG4_LTP;
	else if (caps->object_type & AAC_OBJECT_TYPE_MPEG4_LC)
		caps->object_type = AAC_OBJECT_TYPE_MPEG4_LC;
	else if (caps->object_type & AAC_OBJECT_TYPE_MPEG2_LC)
		caps->object_type = AAC_OBJECT_TYPE_MPEG2_LC;
	else {
		error("AAC: No supported object types: %#x", saved.object_type);
		return errno = ENOTSUP, -1;
	}

	unsigned int ba_bitrate = A2DP_AAC_GET_BITRATE(sep->config.capabilities.aac);
	unsigned int cap_bitrate = A2DP_AAC_GET_BITRATE(*caps);
	if (cap_bitrate == 0)
		/* fix bitrate value if it was not set */
		cap_bitrate = UINT_MAX;
	A2DP_AAC_SET_BITRATE(*caps, MIN(cap_bitrate, ba_bitrate));

	if (!config.aac_prefer_vbr)
		caps->vbr = 0;

	return 0;
}

static int a2dp_aac_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_aac_t *conf = configuration;
	a2dp_aac_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_aac_caps_intersect(&conf_v, &sep->config.capabilities);

	switch (conf_v.object_type) {
	case AAC_OBJECT_TYPE_MPEG2_LC:
	case AAC_OBJECT_TYPE_MPEG4_LC:
	case AAC_OBJECT_TYPE_MPEG4_LTP:
	case AAC_OBJECT_TYPE_MPEG4_SCA:
	case AAC_OBJECT_TYPE_MPEG4_HE:
	case AAC_OBJECT_TYPE_MPEG4_HE2:
	case AAC_OBJECT_TYPE_MPEG4_ELD2:
		break;
	default:
		debug("AAC: Invalid object type: %#x", conf->object_type);
		return A2DP_CHECK_ERR_OBJECT_TYPE;
	}

	const uint16_t conf_sampling_freq = A2DP_AAC_GET_SAMPLING_FREQ(conf_v);
	if (a2dp_bit_mapping_lookup(a2dp_aac_rates, conf_sampling_freq) == -1) {
		debug("AAC: Invalid sample rate: %#x", A2DP_AAC_GET_SAMPLING_FREQ(*conf));
		return A2DP_CHECK_ERR_RATE;
	}

	if (a2dp_bit_mapping_lookup(a2dp_aac_channels, conf_v.channel_mode) == -1) {
		debug("AAC: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_aac_transport_init(struct ba_transport *t) {

	ssize_t channels_i;
	if ((channels_i = a2dp_bit_mapping_lookup(a2dp_aac_channels,
					t->media.configuration.aac.channel_mode)) == -1)
		return -1;

	ssize_t rate_i;
	if ((rate_i = a2dp_bit_mapping_lookup(a2dp_aac_rates,
					A2DP_AAC_GET_SAMPLING_FREQ(t->media.configuration.aac))) == -1)
		return -1;

	t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->media.pcm.channels = a2dp_aac_channels[channels_i].value;
	t->media.pcm.rate = a2dp_aac_rates[rate_i].value;

	memcpy(t->media.pcm.channel_map, a2dp_aac_channels[channels_i].ch.map,
			t->media.pcm.channels * sizeof(*t->media.pcm.channel_map));

	return 0;
}

static int a2dp_aac_source_init(struct a2dp_sep *sep) {

	LIB_INFO info[FDK_MODULE_LAST];
	FDKinitLibInfo(info);
	aacEncGetLibInfo(info);

	unsigned int caps_aac = FDKlibInfo_getCapabilities(info, FDK_AACENC);
	unsigned int caps_sbr = FDKlibInfo_getCapabilities(info, FDK_SBRENC);
	debug("FDK-AAC encoder capabilities: aac=%#x sbr=%#x", caps_aac, caps_sbr);

	/* Check whether mandatory AAC profile is supported. */
	if ((caps_aac & CAPF_AAC_LC) == 0) {
		error("AAC: Low Complexity (AAC-LC) is not supported");
		return errno = ENOTSUP, -1;
	}

	if (caps_aac & CAPF_ER_AAC_SCAL)
		sep->config.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_SCA;
	if (caps_sbr & CAPF_SBR_HQ)
		sep->config.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_HE;
	if (caps_sbr & CAPF_SBR_PS_MPEG)
		sep->config.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_HE2;
	if (caps_aac & CAPF_ER_AAC_ELDV2)
		sep->config.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_ELD2;
	if (caps_aac & CAPF_AAC_UNIDRC)
		sep->config.capabilities.aac.drc = 1;

	if (config.a2dp.force_mono)
		sep->config.capabilities.aac.channel_mode = AAC_CHANNEL_MODE_MONO;
	if (config.a2dp.force_44100)
		A2DP_AAC_SET_SAMPLING_FREQ(sep->config.capabilities.aac, AAC_SAMPLING_FREQ_44100);

	if (!config.aac_prefer_vbr)
		sep->config.capabilities.aac.vbr = 0;

	A2DP_AAC_SET_BITRATE(sep->config.capabilities.aac, config.aac_bitrate);

	return 0;
}

static int a2dp_aac_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_aac_enc_thread, "ba-a2dp-aac");
}

struct a2dp_sep a2dp_aac_source = {
	.name = "A2DP Source (AAC)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_MPEG24,
		.caps_size = sizeof(a2dp_aac_t),
		.capabilities.aac = {
			/* NOTE: AAC Long Term Prediction and AAC Scalable might be
			 *       not supported by the FDK-AAC library. */
			.object_type =
				AAC_OBJECT_TYPE_MPEG2_LC |
				AAC_OBJECT_TYPE_MPEG4_LC,
			A2DP_AAC_INIT_SAMPLING_FREQ(
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
			.channel_mode =
				AAC_CHANNEL_MODE_MONO |
				AAC_CHANNEL_MODE_STEREO |
				AAC_CHANNEL_MODE_5_1 |
				AAC_CHANNEL_MODE_7_1,
			.vbr = 1,
			A2DP_AAC_INIT_BITRATE(320000)
		},
	},
	.init = a2dp_aac_source_init,
	.configuration_select = a2dp_aac_configuration_select,
	.configuration_check = a2dp_aac_configuration_check,
	.transport_init = a2dp_aac_transport_init,
	.transport_start = a2dp_aac_source_transport_start,
	.caps_helpers = &a2dp_aac_caps_helpers,
	.enabled = true,
};

static int a2dp_aac_sink_init(struct a2dp_sep *sep) {

	LIB_INFO info[FDK_MODULE_LAST];
	FDKinitLibInfo(info);
	aacDecoder_GetLibInfo(info);

	unsigned int caps_aac = FDKlibInfo_getCapabilities(info, FDK_AACDEC);
	unsigned int caps_sbr = FDKlibInfo_getCapabilities(info, FDK_SBRDEC);
	unsigned int caps_dmx = FDKlibInfo_getCapabilities(info, FDK_PCMDMX);
	debug("FDK-AAC decoder capabilities: aac=%#x sbr=%#x dmx=%#x",
			caps_aac, caps_sbr, caps_dmx);

	/* Check whether mandatory AAC profile is supported. */
	if ((caps_aac & CAPF_AAC_LC) == 0) {
		error("AAC: Low Complexity (AAC-LC) is not supported");
		return errno = ENOTSUP, -1;
	}

	if (caps_aac & CAPF_ER_AAC_SCAL)
		sep->config.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_SCA;
	if (caps_sbr & CAPF_SBR_HQ)
		sep->config.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_HE;
	if (caps_sbr & CAPF_SBR_PS_MPEG)
		sep->config.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_HE2;
	if (caps_aac & CAPF_ER_AAC_ELDV2)
		sep->config.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_ELD2;
	if (caps_aac & CAPF_AAC_UNIDRC)
		sep->config.capabilities.aac.drc = 1;
	if (caps_dmx & CAPF_DMX_6_CH)
		sep->config.capabilities.aac.channel_mode |= AAC_CHANNEL_MODE_5_1;
	if (caps_dmx & CAPF_DMX_8_CH)
		sep->config.capabilities.aac.channel_mode |= AAC_CHANNEL_MODE_7_1;

	A2DP_AAC_SET_BITRATE(sep->config.capabilities.aac, config.aac_bitrate);

	return 0;
}

static int a2dp_aac_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_aac_dec_thread, "ba-a2dp-aac");
}

struct a2dp_sep a2dp_aac_sink = {
	.name = "A2DP Sink (AAC)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_MPEG24,
		.caps_size = sizeof(a2dp_aac_t),
		.capabilities.aac = {
			/* NOTE: AAC Long Term Prediction and AAC Scalable might be
			 *       not supported by the FDK-AAC library. */
			.object_type =
				AAC_OBJECT_TYPE_MPEG2_LC |
				AAC_OBJECT_TYPE_MPEG4_LC,
			A2DP_AAC_INIT_SAMPLING_FREQ(
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
			/* NOTE: Other channel modes might be not supported
			 *       by the FDK-AAC library. */
			.channel_mode =
				AAC_CHANNEL_MODE_MONO |
				AAC_CHANNEL_MODE_STEREO,
			.vbr = 1,
			A2DP_AAC_INIT_BITRATE(320000)
		},
	},
	.init = a2dp_aac_sink_init,
	.configuration_select = a2dp_aac_configuration_select,
	.configuration_check = a2dp_aac_configuration_check,
	.transport_init = a2dp_aac_transport_init,
	.transport_start = a2dp_aac_sink_transport_start,
	.caps_helpers = &a2dp_aac_caps_helpers,
	.enabled = true,
};
