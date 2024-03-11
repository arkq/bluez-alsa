/*
 * BlueALSA - a2dp-lhdc.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 * Copyright (c) 2023      anonymix007
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-lhdc.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <glib.h>

#include <lhdcBT.h>
#include <lhdcBT_dec.h>

#include "a2dp.h"
#include "audio.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "ba-config.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static LHDC_VERSION_SETUP get_version(const a2dp_lhdc_v3_t *configuration) {
	if (configuration->llac) {
		return LLAC;
	} else if (configuration->lhdc_v4) {
		return LHDC_V4;
	} else {
		return LHDC_V3;
	}
}

static int get_encoder_interval(const a2dp_lhdc_v3_t *configuration) {
	if (configuration->low_latency) {
		return 10;
	} else {
		return 20;
	}
}

static int get_bit_depth(const a2dp_lhdc_v3_t *configuration) {
	if (configuration->bit_depth == LHDC_BIT_DEPTH_16) {
		return 16;
	} else {
		return 24;
	}
}

static LHDCBT_QUALITY_T get_max_bitrate(const a2dp_lhdc_v3_t *configuration) {
	if (configuration->max_bitrate == LHDC_MAX_BITRATE_400K) {
		return LHDCBT_QUALITY_LOW;
	} else if (configuration->max_bitrate == LHDC_MAX_BITRATE_500K) {
		return LHDCBT_QUALITY_MID;
	} else {
		return LHDCBT_QUALITY_HIGH;
	}
}

void *a2dp_lhdc_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	const a2dp_lhdc_v3_t *configuration = &t->a2dp.configuration.lhdc_v3;

	HANDLE_LHDC_BT handle;
	if ((handle = lhdcBT_get_handle(get_version(configuration))) == NULL) {
		error("Couldn't get LHDC handle: %s", strerror(errno));
		goto fail_open_lhdc;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(lhdcBT_free_handle), handle);

	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;
	const unsigned int bitdepth = get_bit_depth(configuration);

	lhdcBT_set_hasMinBitrateLimit(handle, configuration->min_bitrate);
	lhdcBT_set_max_bitrate(handle, get_max_bitrate(configuration));

	if (lhdcBT_init_encoder(handle, samplerate, bitdepth, config.lhdc_eqmid,
			configuration->ch_split_mode > LHDC_CH_SPLIT_MODE_NONE, 0, t->mtu_write,
			get_encoder_interval(configuration)) == -1) {
		error("Couldn't initialize LHDC encoder");
		goto fail_init;
	}

	const size_t lhdc_ch_samples = lhdcBT_get_block_Size(handle);
	const size_t lhdc_pcm_samples = lhdc_ch_samples * channels;

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	int32_t *pcm_ch1 = malloc(lhdc_ch_samples * sizeof(int32_t));
	int32_t *pcm_ch2 = malloc(lhdc_ch_samples * sizeof(int32_t));
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch1);
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch2);

	if (ffb_init_int32_t(&pcm, lhdc_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	struct {
		uint8_t seq_num;
		uint8_t latency:2;
		uint8_t frames:6;
	} *lhdc_media_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&lhdc_media_header, sizeof(*lhdc_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio samplerate */
	rtp_state_init(&rtp, samplerate, samplerate);

	uint8_t seq_num = 0;

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t samples = ffb_len_in(&pcm);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, pcm.tail)) {
		case -1:
			if (errno == ESTALE) {
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
		samples = ffb_len_out(&pcm);

		int *input = pcm.data;
		size_t input_len = samples;

		/* encode and transfer obtained data */
		while (input_len >= lhdc_pcm_samples) {

			/* anchor for RTP payload */
			bt.tail = rtp_payload;

			audio_deinterleave_s32_4le(input, lhdc_ch_samples, channels, pcm_ch1, pcm_ch2);

			uint32_t encoded;
			uint32_t frames;

			if (lhdcBT_encode_stereo(handle, pcm_ch1, pcm_ch2, bt.tail, &encoded, &frames) < 0) {
				error("LHDC encoding error");
				break;
			}

			input += lhdc_pcm_samples;
			input_len -= lhdc_pcm_samples;
			ffb_seek(&bt, encoded);

			if (encoded > 0) {

				lhdc_media_header->seq_num = seq_num++;
				lhdc_media_header->latency = 0;
				lhdc_media_header->frames = frames;

				rtp_state_new_frame(&rtp, rtp_header);

				/* Try to get the number of bytes queued in the
				 * socket output buffer. */
				int queued_bytes = 0;
				if (ioctl(t->bt_fd, TIOCOUTQ, &queued_bytes) != -1)
					queued_bytes = abs(t->a2dp.bt_fd_coutq_init - queued_bytes);

				errno = 0;

				ssize_t len = ffb_blen_out(&bt);
				if ((len = io_bt_write(t_pcm, bt.data, len)) <= 0) {
					if (len == -1)
						error("BT write error: %s", strerror(errno));
					goto fail;
				}

				if (errno == EAGAIN)
					/* The io_bt_write() call was blocking due to not enough
					 * space in the BT socket. Set the queued_bytes to some
					 * arbitrary big value. */
					queued_bytes = 1024 * 16;

				if (config.lhdc_eqmid == LHDCBT_QUALITY_AUTO)
					lhdcBT_adjust_bitrate(handle, queued_bytes / t->mtu_write);
			}

			unsigned int pcm_frames = lhdc_pcm_samples / channels;
			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, pcm_frames);

			/* update busy delay (encoding overhead) */
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

		}

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_len);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open_lhdc:
	pthread_cleanup_pop(1);
	return NULL;
}

static const int versions[5] = {
	[LHDC_V2] = VERSION_2,
	[LHDC_V3] = VERSION_3,
	[LHDC_V4] = VERSION_4,
	[LLAC]	= VERSION_LLAC,
};

void *a2dp_lhdc_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	const a2dp_lhdc_v3_t *configuration = &t->a2dp.configuration.lhdc_v3;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;
	const unsigned int bitdepth = get_bit_depth(configuration);

	tLHDCV3_DEC_CONFIG dec_config = {
		.version = versions[get_version(configuration)],
		.sample_rate = samplerate,
		.bits_depth = bitdepth,
	};

	if (lhdcBT_dec_init_decoder(&dec_config) < 0) {
		error("Couldn't initialise LHDC decoder: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(lhdcBT_dec_deinit_decoder), NULL);

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int32_t(&pcm, 16 * 256 * channels) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio samplerate */
	rtp_state_init(&rtp, samplerate, samplerate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, bt.data)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		const rtp_header_t *rtp_header = bt.data;
		const void *lhdc_media_header;
		if ((lhdc_media_header = rtp_a2dp_get_payload(rtp_header)) == NULL)
			continue;

		int missing_rtp_frames = 0;
		rtp_state_sync_stream(&rtp, rtp_header, &missing_rtp_frames, NULL);

		if (!ba_transport_pcm_is_active(t_pcm)) {
			rtp.synced = false;
			continue;
		}

		const uint8_t *rtp_payload = (uint8_t *) lhdc_media_header;
		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		uint32_t decoded = 16 * 256 * sizeof(int32_t) * channels;

		lhdcBT_dec_decode(rtp_payload, rtp_payload_len, pcm.data, &decoded, 24);

		const size_t samples = decoded / sample_size;
		io_pcm_scale(t_pcm, pcm.data, samples);
		if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		/* update local state with decoded PCM frames */
		rtp_state_update(&rtp, samples / channels);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}

static const struct a2dp_sampling a2dp_lhdc_samplings[] = {
	{ 44100, LHDC_SAMPLING_FREQ_44100 },
	{ 48000, LHDC_SAMPLING_FREQ_48000 },
	{ 96000, LHDC_SAMPLING_FREQ_96000 },
	{ 0 },
};

static int a2dp_lhdc_configuration_select(
		const struct a2dp_codec *codec,
		void *capabilities) {

	warn("LHDC: LLAC/V3/V4 switch logic is not implemented");

	a2dp_lhdc_v3_t *caps = capabilities;
	const a2dp_lhdc_v3_t saved = *caps;

	/* narrow capabilities to values supported by BlueALSA */
	if (a2dp_filter_capabilities(codec, &codec->capabilities,
				caps, sizeof(*caps)) != 0)
		return -1;

	if (caps->bit_depth & LHDC_BIT_DEPTH_24) {
		caps->bit_depth = LHDC_BIT_DEPTH_24;
	} else if (caps->bit_depth & LHDC_BIT_DEPTH_16) {
		caps->bit_depth = LHDC_BIT_DEPTH_16;
	} else {
		error("LHDC: No supported bit depths: %#x", saved.bit_depth);
		return errno = ENOTSUP, -1;
	}

	const struct a2dp_sampling *sampling;
	if ((sampling = a2dp_sampling_select(a2dp_lhdc_samplings, caps->frequency)) != NULL)
		caps->frequency = sampling->value;
	else {
		error("LHDC: No supported sampling frequencies: %#x", saved.frequency);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_lhdc_configuration_check(
		const struct a2dp_codec *codec,
		const void *configuration) {

	const a2dp_lhdc_v3_t *conf = configuration;
	a2dp_lhdc_v3_t conf_v = *conf;

	/* validate configuration against BlueALSA capabilities */
	if (a2dp_filter_capabilities(codec, &codec->capabilities,
				&conf_v, sizeof(conf_v)) != 0)
		return A2DP_CHECK_ERR_SIZE;

	if (a2dp_sampling_lookup(a2dp_lhdc_samplings, conf_v.frequency) == NULL) {
		debug("LHDC: Invalid sampling frequency: %#x", conf->frequency);
		return A2DP_CHECK_ERR_SAMPLING;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_lhdc_transport_init(struct ba_transport *t) {

	const struct a2dp_sampling *sampling;
	if ((sampling = a2dp_sampling_lookup(a2dp_lhdc_samplings,
					t->a2dp.configuration.lhdc_v3.frequency)) == NULL)
		return -1;

	if (t->a2dp.codec->dir == A2DP_SINK) {
		t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S24_4LE;
	} else {
		t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S32_4LE;
	}

	t->a2dp.pcm.channels = 2;
	t->a2dp.pcm.sampling = sampling->frequency;

	return 0;
}

static int a2dp_lhdc_source_init(struct a2dp_codec *codec) {
	if (config.a2dp.force_44100)
		codec->capabilities.lhdc_v3.frequency = LHDC_SAMPLING_FREQ_44100;
	return 0;
}

static int a2dp_lhdc_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_lhdc_enc_thread, "ba-a2dp-lhdc");
}

struct a2dp_codec a2dp_lhdc_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_LHDC_V3,
	.synopsis = "A2DP Source (LHDC V3)",
	.capabilities.lhdc_v3 = {
		.info = A2DP_VENDOR_INFO_INIT(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID),
		.frequency =
			LHDC_SAMPLING_FREQ_44100 |
			LHDC_SAMPLING_FREQ_48000 |
			LHDC_SAMPLING_FREQ_96000,
		.bit_depth =
			LHDC_BIT_DEPTH_16 |
			LHDC_BIT_DEPTH_24,
		.jas = 0,
		.ar = 0,
		.version = LHDC_VER3,
		.max_bitrate = LHDC_MAX_BITRATE_900K,
		.low_latency = 0,
		.llac = 0, // TODO: copy LLAC/V3/V4 logic from AOSP patches
		.ch_split_mode = LHDC_CH_SPLIT_MODE_NONE,
		.meta = 0,
		.min_bitrate = 0,
		.larc = 0,
		.lhdc_v4 = 1,
	},
	.capabilities_size = sizeof(a2dp_lhdc_v3_t),
	.init = a2dp_lhdc_source_init,
	.configuration_select = a2dp_lhdc_configuration_select,
	.configuration_check = a2dp_lhdc_configuration_check,
	.transport_init = a2dp_lhdc_transport_init,
	.transport_start = a2dp_lhdc_source_transport_start,
};

static int a2dp_lhdc_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_lhdc_dec_thread, "ba-a2dp-lhdc");
}

struct a2dp_codec a2dp_lhdc_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_LHDC_V3,
	.synopsis = "A2DP Sink (LHDC V3)",
	.capabilities.lhdc_v3 = {
		.info = A2DP_VENDOR_INFO_INIT(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID),
		.frequency =
			LHDC_SAMPLING_FREQ_44100 |
			LHDC_SAMPLING_FREQ_48000 |
			LHDC_SAMPLING_FREQ_96000,
		.bit_depth =
			LHDC_BIT_DEPTH_16 |
			LHDC_BIT_DEPTH_24,
		.jas = 0,
		.ar = 0,
		.version = LHDC_VER3,
		.max_bitrate = LHDC_MAX_BITRATE_900K,
		.low_latency = 0,
		.llac = 1,
		.ch_split_mode = LHDC_CH_SPLIT_MODE_NONE,
		.meta = 0,
		.min_bitrate = 0,
		.larc = 0,
		.lhdc_v4 = 1,
	},
	.capabilities_size = sizeof(a2dp_lhdc_v3_t),
	.configuration_select = a2dp_lhdc_configuration_select,
	.configuration_check = a2dp_lhdc_configuration_check,
	.transport_init = a2dp_lhdc_transport_init,
	.transport_start = a2dp_lhdc_sink_transport_start,
};
