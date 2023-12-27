/*
 * BlueALSA - a2dp-aac.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-aac.h"

#include <errno.h>
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
#include "ba-transport-pcm.h"
#include "bluealsa-config.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

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

struct a2dp_codec a2dp_aac_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_MPEG24,
	.capabilities.aac = {
		/* NOTE: AAC Long Term Prediction and AAC Scalable might be
		 *       not supported by the FDK-AAC library. */
		.object_type =
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
	},
	.capabilities_size = sizeof(a2dp_aac_t),
	.channels[0] = a2dp_aac_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aac_channels),
	.samplings[0] = a2dp_aac_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aac_samplings),
	.enabled = true,
};

struct a2dp_codec a2dp_aac_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_MPEG24,
	.capabilities.aac = {
		/* NOTE: AAC Long Term Prediction and AAC Scalable might be
		 *       not supported by the FDK-AAC library. */
		.object_type =
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
	},
	.capabilities_size = sizeof(a2dp_aac_t),
	.channels[0] = a2dp_aac_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aac_channels),
	.samplings[0] = a2dp_aac_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aac_samplings),
	.enabled = true,
};

void a2dp_aac_init(void) {

	LIB_INFO info[16] = { 0 };
	info[ARRAYSIZE(info) - 1].module_id = ~FDK_NONE;

	aacDecoder_GetLibInfo(info);
	aacEncGetLibInfo(info);

	unsigned int caps_dec = FDKlibInfo_getCapabilities(info, FDK_AACDEC);
	unsigned int caps_enc = FDKlibInfo_getCapabilities(info, FDK_AACENC);
	debug("FDK-AAC lib capabilities: dec:%#x enc:%#x", caps_dec, caps_enc);

	if (caps_dec & CAPF_ER_AAC_SCAL)
		a2dp_aac_sink.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_AAC_SCA;
	if (caps_enc & CAPF_ER_AAC_SCAL)
		a2dp_aac_source.capabilities.aac.object_type |= AAC_OBJECT_TYPE_MPEG4_AAC_SCA;

	if (config.a2dp.force_mono)
		a2dp_aac_source.capabilities.aac.channels = AAC_CHANNELS_1;
	if (config.a2dp.force_44100)
		AAC_SET_FREQUENCY(a2dp_aac_source.capabilities.aac, AAC_SAMPLING_FREQ_44100);

	if (!config.aac_prefer_vbr)
		a2dp_aac_source.capabilities.aac.vbr = 0;

	AAC_SET_BITRATE(a2dp_aac_source.capabilities.aac, config.aac_bitrate);
	AAC_SET_BITRATE(a2dp_aac_sink.capabilities.aac, config.aac_bitrate);

}

void a2dp_aac_transport_init(struct ba_transport *t) {

	const struct a2dp_codec *codec = t->a2dp.codec;

	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
			t->a2dp.configuration.aac.channels, false);
	t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
			AAC_GET_FREQUENCY(t->a2dp.configuration.aac), false);

}

static unsigned int a2dp_aac_get_fdk_vbr_mode(
		unsigned int channelmode, unsigned int bitrate) {
	static const unsigned int modes[][5] = {
		/* bitrate upper bounds for mono channel mode */
		{ 32000, 40000, 56000, 72000, 112000 },
		/* bitrate upper bounds for stereo channel mode */
		{ 40000, 64000, 96000, 128000, 192000 },
	};
	const size_t ch = channelmode == MODE_1 ? 0 : 1;
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
	AACENC_InfoStruct aacinf;
	AACENC_ERROR err;

	const a2dp_aac_t *configuration = &t->a2dp.configuration.aac;
	const unsigned int bitrate = AAC_GET_BITRATE(*configuration);
	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;

	/* create AAC encoder without the Meta Data module */
	if ((err = aacEncOpen(&handle, 0x07, channels)) != AACENC_OK) {
		error("Couldn't open AAC encoder: %s", aacenc_strerror(err));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(aacEncClose), &handle);

	unsigned int aot = AOT_NONE;
	unsigned int channelmode = channels == 1 ? MODE_1 : MODE_2;

	switch (configuration->object_type) {
	case AAC_OBJECT_TYPE_MPEG2_AAC_LC:
#if AACENCODER_LIB_VERSION <= 0x03040C00 /* 3.4.12 */ || \
		AACENCODER_LIB_VERSION >= 0x04000000 /* 4.0.0 */
		aot = AOT_MP2_AAC_LC;
		break;
#endif
	case AAC_OBJECT_TYPE_MPEG4_AAC_LC:
		aot = AOT_AAC_LC;
		break;
	case AAC_OBJECT_TYPE_MPEG4_AAC_LTP:
		aot = AOT_AAC_LTP;
		break;
	case AAC_OBJECT_TYPE_MPEG4_AAC_SCA:
		aot = AOT_AAC_SCAL;
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
	if ((err = aacEncoder_SetParam(handle, AACENC_SAMPLERATE, samplerate)) != AACENC_OK) {
		error("Couldn't set sampling rate: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_CHANNELMODE, channelmode)) != AACENC_OK) {
		error("Couldn't set channel mode: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if (configuration->vbr) {
		const unsigned int mode = a2dp_aac_get_fdk_vbr_mode(channelmode, bitrate);
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
	if ((err = aacEncInfo(handle, &aacinf)) != AACENC_OK) {
		error("Couldn't get encoder info: %s", aacenc_strerror(err));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const unsigned int aac_frame_size = aacinf.inputChannels * aacinf.frameLength;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	if (ffb_init(&pcm, aac_frame_size, sample_size) == -1 ||
			ffb_init_uint8_t(&bt, RTP_HEADER_LEN + aacinf.maxOutBufBytes) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	/* initialize RTP header and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header, NULL, 0);

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to 90kHz */
	rtp_state_init(&rtp, samplerate, 90000);

	int in_bufferIdentifiers[] = { IN_AUDIO_DATA };
	int out_bufferIdentifiers[] = { OUT_BITSTREAM_DATA };
	int in_bufSizes[] = { pcm.nmemb * pcm.size };
	int out_bufSizes[] = { aacinf.maxOutBufBytes };
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

		ssize_t samples = ffb_len_in(&pcm);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, pcm.tail, samples)) {
		case -1:
			if (errno == ESTALE) {
				in_args.numInSamples = -1;
				/* flush encoder internal buffers */
				while (aacEncEncode(handle, NULL, &out_buf, &in_args, &out_args) == AACENC_OK)
					continue;
				ffb_rewind(&pcm);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		ffb_seek(&pcm, samples);
		while ((in_args.numInSamples = ffb_len_out(&pcm)) > 0) {

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

			unsigned int pcm_frames = out_args.numInSamples / channels;
			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, pcm_frames);

			/* update busy delay (encoding overhead) */
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

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
	const unsigned int samplerate = t_pcm->sampling;

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
	rtp_state_init(&rtp, samplerate, 90000);

	int markbit_quirk = -3;

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, bt.data, len)) <= 0) {
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
		CStreamInfo *aacinf;

		if ((err = aacDecoder_Fill(handle, (uint8_t **)&latm.data, &data_len, &valid)) != AAC_DEC_OK)
			error("AAC buffer fill error: %s", aacdec_strerror(err));
		else if ((err = aacDecoder_DecodeFrame(handle, pcm.tail, ffb_blen_in(&pcm), 0)) != AAC_DEC_OK)
			error("AAC decode frame error: %s", aacdec_strerror(err));
		else if ((aacinf = aacDecoder_GetStreamInfo(handle)) == NULL)
			error("Couldn't get AAC stream info");
		else {

			if ((unsigned int)aacinf->numChannels != channels)
				warn("AAC channels mismatch: %u != %u", aacinf->numChannels, channels);

			const size_t samples = (size_t)aacinf->frameSize * channels;
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

			/* update local state with decoded PCM frames */
			rtp_state_update(&rtp, aacinf->frameSize);

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

int a2dp_aac_transport_start(struct ba_transport *t) {

	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_aac_enc_thread, "ba-a2dp-aac");

	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_aac_dec_thread, "ba-a2dp-aac");

	g_assert_not_reached();
	return -1;
}
