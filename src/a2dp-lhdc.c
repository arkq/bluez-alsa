/*
 * BlueALSA - a2dp-lhdc.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
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

#include <lhdcBT.h>
#include <lhdcBT_dec.h>

#include "a2dp.h"
#include "audio.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "ba-config.h"
#include "bluealsa-dbus.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_bit_mapping a2dp_lhdc_rates[] = {
	{ LHDC_SAMPLING_FREQ_44100, { 44100 } },
	{ LHDC_SAMPLING_FREQ_48000, { 48000 } },
	{ LHDC_SAMPLING_FREQ_96000, { 96000 } },
	{ 0 },
};

static void a2dp_lhdc_v2_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_lhdc_v2_t));
}

static void a2dp_lhdc_v3_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_lhdc_v3_t));
}

static void a2dp_lhdc_v5_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_lhdc_v5_t));
}

static int a2dp_lhdc_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	(void)capabilities;
	static const struct a2dp_bit_mapping channels_stereo = {
		.ch = { 2, a2dp_channel_map_stereo } };
	if (stream == A2DP_MAIN)
		return func(channels_stereo, userdata);
	return -1;
}

static int a2dp_lhdc_v2_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_lhdc_v2_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_lhdc_rates, caps->sampling_freq, func, userdata);
	return -1;
}

static int a2dp_lhdc_v3_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_lhdc_v3_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_lhdc_rates, caps->sampling_freq, func, userdata);
	return -1;
}

static int a2dp_lhdc_v5_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_lhdc_v5_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_lhdc_rates, caps->sampling_freq, func, userdata);
	return -1;
}

static void a2dp_lhdc_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	(void)capabilities;
	(void)stream;
	(void)channels;
}

static void a2dp_lhdc_v2_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_lhdc_v2_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_lhdc_rates,
				caps->sampling_freq, rate);
}

static void a2dp_lhdc_v3_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_lhdc_v3_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_lhdc_rates,
				caps->sampling_freq, rate);
}

static void a2dp_lhdc_v5_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_lhdc_v5_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_lhdc_rates,
				caps->sampling_freq, rate);
}

static struct a2dp_caps_helpers a2dp_lhdc_v2_caps_helpers = {
	.intersect = a2dp_lhdc_v2_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_lhdc_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_lhdc_v2_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_lhdc_caps_select_channel_mode,
	.select_sample_rate = a2dp_lhdc_v2_caps_select_sample_rate,
};

static struct a2dp_caps_helpers a2dp_lhdc_v3_caps_helpers = {
	.intersect = a2dp_lhdc_v3_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_lhdc_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_lhdc_v3_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_lhdc_caps_select_channel_mode,
	.select_sample_rate = a2dp_lhdc_v3_caps_select_sample_rate,
};

static struct a2dp_caps_helpers a2dp_lhdc_v5_caps_helpers = {
	.intersect = a2dp_lhdc_v5_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_lhdc_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_lhdc_v5_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_lhdc_caps_select_channel_mode,
	.select_sample_rate = a2dp_lhdc_v5_caps_select_sample_rate,
};

static LHDC_VERSION_SETUP get_lhdc_enc_version(const void *configuration) {
	switch (((a2dp_vendor_info_t *)configuration)->codec_id) {
	case LHDC_V2_CODEC_ID:
		return LHDC_V2;
	case LHDC_V3_CODEC_ID: {
		const a2dp_lhdc_v3_t *v3 = configuration;
		if (v3->llac)
			return LLAC;
		if (v3->lhdc_v4)
			return LHDC_V4;
		return LHDC_V3;
	} break;
	default:
		return 0;
	}
}

static lhdc_ver_t get_lhdc_dec_version(const void *configuration) {

	static const lhdc_ver_t versions[] = {
		[LHDC_V2] = VERSION_2,
		[LHDC_V3] = VERSION_3,
		[LHDC_V4] = VERSION_4,
		[LLAC]	= VERSION_LLAC,
	};

	return versions[get_lhdc_enc_version(configuration)];
}

static LHDCBT_QUALITY_T get_lhdc_max_bitrate(uint8_t config_max_bitrate) {
	switch (config_max_bitrate) {
	case LHDC_MAX_BITRATE_400K:
		return LHDCBT_QUALITY_LOW;
	case LHDC_MAX_BITRATE_500K:
		return LHDCBT_QUALITY_MID;
	case LHDC_MAX_BITRATE_900K:
	default:
		return LHDCBT_QUALITY_HIGH;
	}
}

void *a2dp_lhdc_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	const uint32_t codec_id = ba_transport_get_codec(t);
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	HANDLE_LHDC_BT handle;
	if ((handle = lhdcBT_get_handle(get_lhdc_enc_version(&t->media.configuration))) == NULL) {
		error("Couldn't get LHDC handle: %s", strerror(errno));
		goto fail_open_lhdc;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(lhdcBT_free_handle), handle);

	int lhdc_max_bitrate_index = 0;
	int lhdc_bit_depth = 0;
	int lhdc_dual_channel = 0;
	int lhdc_interval = 0;

	switch (codec_id) {
	case A2DP_CODEC_VENDOR_ID(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID):
		error("LHDC v2 is not supported yet");
		goto fail_init;
	case A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID):
		lhdcBT_set_hasMinBitrateLimit(handle, t->media.configuration.lhdc_v3.min_bitrate);
		lhdc_max_bitrate_index = get_lhdc_max_bitrate(t->media.configuration.lhdc_v3.max_bitrate);
		lhdc_bit_depth = t->media.configuration.lhdc_v3.bit_depth == LHDC_BIT_DEPTH_16 ? 16 : 24;
		lhdc_dual_channel = t->media.configuration.lhdc_v3.ch_split_mode > LHDC_CH_SPLIT_MODE_NONE;
		lhdc_interval = t->media.configuration.lhdc_v3.low_latency ? 10 : 20;
		break;
	case A2DP_CODEC_VENDOR_ID(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID):
		error("LHDC v5 is not supported yet");
		goto fail_init;
	}

	lhdcBT_set_max_bitrate(handle, lhdc_max_bitrate_index);

	if (lhdcBT_init_encoder(handle, rate, lhdc_bit_depth, config.lhdc_eqmid, lhdc_dual_channel,
				0, t->mtu_write - RTP_HEADER_LEN - sizeof(rtp_lhdc_media_header_t),
				lhdc_interval) == -1) {
		error("Couldn't initialize LHDC encoder");
		goto fail_init;
	}

	const size_t lhdc_frame_pcm_frames = lhdcBT_get_block_Size(handle);
	const size_t lhdc_frame_pcm_samples = lhdc_frame_pcm_frames * channels;

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	int32_t *pcm_ch1 = malloc(lhdc_frame_pcm_frames * sizeof(int32_t));
	int32_t *pcm_ch2 = malloc(lhdc_frame_pcm_frames * sizeof(int32_t));
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch1);
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch2);

	if (ffb_init_int32_t(&pcm, lhdc_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1 ||
			pcm_ch1 == NULL || pcm_ch2 == NULL) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	const unsigned int lhdc_delay_pcm_frames = 1024;
	/* Get the total delay introduced by the codec. */
	t_pcm->codec_delay_dms = lhdc_delay_pcm_frames * 10000 / rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

	rtp_header_t *rtp_header;
	rtp_lhdc_media_header_t *rtp_lhdc_media_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_lhdc_media_header, sizeof(*rtp_lhdc_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to PCM sample rate */
	rtp_state_init(&rtp, rate, rate);

	uint8_t seq_num = 0;

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				/* TODO: flush encoder internal buffers */
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		int32_t *input = pcm.data;
		ssize_t samples = ffb_len_out(&pcm);
		size_t input_len = samples;

		/* Encode and transfer obtained data. */
		while (input_len >= lhdc_frame_pcm_samples) {

			/* anchor for RTP payload */
			bt.tail = rtp_payload;

			int32_t *pcm_ch_buffers[2] = { pcm_ch1, pcm_ch2 };
			audio_deinterleave_s24_4le(pcm_ch_buffers, input, channels, lhdc_frame_pcm_frames);

			uint32_t encoded;
			uint32_t frames;

			int rv;
			if ((rv = lhdcBT_encode_stereo(handle, pcm_ch1, pcm_ch2, bt.tail, &encoded, &frames)) < 0) {
				error("LHDC encoding error: %d", rv);
				break;
			}

			input += lhdc_frame_pcm_samples;
			input_len -= lhdc_frame_pcm_samples;
			ffb_seek(&bt, encoded);

			if (encoded > 0) {

				rtp_state_new_frame(&rtp, rtp_header);

				rtp_lhdc_media_header->latency = 0;
				rtp_lhdc_media_header->frame_count = frames;
				rtp_lhdc_media_header->seq_number = seq_num++;

				/* Try to get the number of bytes queued in the
				 * socket output buffer. */
				int queued_bytes = 0;
				if (ioctl(t->bt_fd, TIOCOUTQ, &queued_bytes) != -1)
					queued_bytes = abs(t->media.bt_fd_coutq_init - queued_bytes);

				errno = 0;

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

				if (errno == EAGAIN)
					/* The io_bt_write() call was blocking due to not enough
					 * space in the BT socket. Set the queued_bytes to some
					 * arbitrary big value. */
					queued_bytes = 1024 * 16;

				if (config.lhdc_eqmid == LHDCBT_QUALITY_AUTO)
					lhdcBT_adjust_bitrate(handle, queued_bytes / t->mtu_write);

			}

			const size_t pcm_frames = lhdc_frame_pcm_samples / channels;
			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, pcm_frames);

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

void *a2dp_lhdc_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	tLHDCV3_DEC_CONFIG dec_config = { .sample_rate = rate };

	switch (ba_transport_get_codec(t)) {
	case A2DP_CODEC_VENDOR_ID(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID):
		error("LHDC v2 is not supported yet");
		goto fail_open;
	case A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID):
		dec_config.version = get_lhdc_dec_version(&t->media.configuration.lhdc_v3);
		dec_config.bits_depth = t->media.configuration.lhdc_v3.bit_depth == LHDC_BIT_DEPTH_16 ? 16 : 24;
		break;
	case A2DP_CODEC_VENDOR_ID(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID):
		error("LHDC v5 is not supported yet");
		goto fail_open;
	}

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
	/* RTP clock frequency equal to PCM sample rate */
	rtp_state_init(&rtp, rate, rate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len;
		ffb_rewind(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		const rtp_header_t *rtp_header = bt.data;
		const rtp_lhdc_media_header_t *rtp_lhdc_media_header;
		if ((rtp_lhdc_media_header = rtp_a2dp_get_payload(rtp_header)) == NULL)
			continue;

		int missing_rtp_frames = 0;
		rtp_state_sync_stream(&rtp, rtp_header, &missing_rtp_frames, NULL);

		if (!ba_transport_pcm_is_active(t_pcm)) {
			rtp.synced = false;
			continue;
		}

		const uint8_t *rtp_payload = (uint8_t *)rtp_lhdc_media_header;
		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		int rv;
		uint32_t decoded = ffb_blen_in(&pcm);
		if ((rv = lhdcBT_dec_decode(rtp_payload, rtp_payload_len, pcm.data, &decoded, 24)) != 0) {
			error("LHDC decoding error: %s", lhdcBT_dec_strerror(rv));
			continue;
		}

		const size_t samples = decoded / sample_size;

		/* Upscale decoded 24-bit PCM samples to 32-bit. */
		for (size_t i = 0; i < samples; i++)
			((int32_t *)pcm.data)[i] <<= 8;

		io_pcm_scale(t_pcm, pcm.data, samples);
		if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
			error("PCM write error: %s", strerror(errno));

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

static int a2dp_lhdc_v2_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_lhdc_v2_t *caps = capabilities;
	const a2dp_lhdc_v2_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_lhdc_v2_caps_intersect(caps, &sep->config.capabilities);

	if (caps->bit_depth & LHDC_BIT_DEPTH_24)
		caps->bit_depth = LHDC_BIT_DEPTH_24;
	else if (caps->bit_depth & LHDC_BIT_DEPTH_16)
		caps->bit_depth = LHDC_BIT_DEPTH_16;
	else {
		error("LHDC: No supported bit depths: %#x", saved.bit_depth);
		return errno = ENOTSUP, -1;
	}

	unsigned int sampling_freq = 0;
	if (a2dp_lhdc_v2_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		caps->sampling_freq = sampling_freq;
	else {
		error("LHDC: No supported sample rates: %#x", saved.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_lhdc_v3_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	warn("LHDC: LLAC/V3/V4 switch logic is not implemented");

	a2dp_lhdc_v3_t *caps = capabilities;
	const a2dp_lhdc_v3_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_lhdc_v3_caps_intersect(caps, &sep->config.capabilities);

	if (caps->bit_depth & LHDC_BIT_DEPTH_24)
		caps->bit_depth = LHDC_BIT_DEPTH_24;
	else if (caps->bit_depth & LHDC_BIT_DEPTH_16)
		caps->bit_depth = LHDC_BIT_DEPTH_16;
	else {
		error("LHDC: No supported bit depths: %#x", saved.bit_depth);
		return errno = ENOTSUP, -1;
	}

	unsigned int sampling_freq = 0;
	if (a2dp_lhdc_v3_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		caps->sampling_freq = sampling_freq;
	else {
		error("LHDC: No supported sample rates: %#x", saved.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_lhdc_v5_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_lhdc_v5_t *caps = capabilities;
	const a2dp_lhdc_v5_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_lhdc_v5_caps_intersect(caps, &sep->config.capabilities);

	if (caps->bit_depth & LHDC_BIT_DEPTH_24)
		caps->bit_depth = LHDC_BIT_DEPTH_24;
	else if (caps->bit_depth & LHDC_BIT_DEPTH_16)
		caps->bit_depth = LHDC_BIT_DEPTH_16;
	else {
		error("LHDC: No supported bit depths: %#x", saved.bit_depth);
		return errno = ENOTSUP, -1;
	}

	unsigned int sampling_freq = 0;
	if (a2dp_lhdc_v5_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		caps->sampling_freq = sampling_freq;
	else {
		error("LHDC: No supported sample rates: %#x", saved.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_lhdc_v2_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_lhdc_v2_t *conf = configuration;
	a2dp_lhdc_v2_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_lhdc_v2_caps_intersect(&conf_v, &sep->config.capabilities);

	if (a2dp_bit_mapping_lookup(a2dp_lhdc_rates, conf_v.sampling_freq) == -1) {
		debug("LHDC: Invalid sample rate: %#x", conf->sampling_freq);
		return A2DP_CHECK_ERR_RATE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_lhdc_v3_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_lhdc_v3_t *conf = configuration;
	a2dp_lhdc_v3_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_lhdc_v3_caps_intersect(&conf_v, &sep->config.capabilities);

	if (a2dp_bit_mapping_lookup(a2dp_lhdc_rates, conf_v.sampling_freq) == -1) {
		debug("LHDC: Invalid sample rate: %#x", conf->sampling_freq);
		return A2DP_CHECK_ERR_RATE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_lhdc_v5_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_lhdc_v5_t *conf = configuration;
	a2dp_lhdc_v5_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_lhdc_v5_caps_intersect(&conf_v, &sep->config.capabilities);

	if (a2dp_bit_mapping_lookup(a2dp_lhdc_rates, conf_v.sampling_freq) == -1) {
		debug("LHDC: Invalid sample rate: %#x", conf->sampling_freq);
		return A2DP_CHECK_ERR_RATE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_lhdc_transport_init(struct ba_transport *t) {

	ssize_t rate_i;
	switch (t->codec_id) {
	case A2DP_CODEC_VENDOR_ID(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID):
		if ((rate_i = a2dp_bit_mapping_lookup(a2dp_lhdc_rates,
						t->media.configuration.lhdc_v2.sampling_freq)) == -1)
			return -1;
		break;
	case A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID):
		if ((rate_i = a2dp_bit_mapping_lookup(a2dp_lhdc_rates,
						t->media.configuration.lhdc_v3.sampling_freq)) == -1)
			return -1;
		break;
	case A2DP_CODEC_VENDOR_ID(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID):
		if ((rate_i = a2dp_bit_mapping_lookup(a2dp_lhdc_rates,
						t->media.configuration.lhdc_v5.sampling_freq)) == -1)
			return -1;
		break;
	default:
		return -1;
	}

	/* LHDC library uses 32-bit signed integers for the encoder API and
	 * 24-bit signed integers for the decoder API. So, the best common
	 * choice for PCM sample is signed 32-bit. */
	t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S32_4LE;
	t->media.pcm.channels = 2;
	t->media.pcm.rate = a2dp_lhdc_rates[rate_i].value;

	memcpy(t->media.pcm.channel_map, a2dp_channel_map_stereo,
			2 * sizeof(*a2dp_channel_map_stereo));

	return 0;
}

static int a2dp_lhdc_source_init(struct a2dp_sep *sep) {
	if (config.a2dp.force_44100)
		switch (sep->config.codec_id) {
		case A2DP_CODEC_VENDOR_ID(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID):
			sep->config.capabilities.lhdc_v2.sampling_freq = LHDC_SAMPLING_FREQ_44100;
			break;
		case A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID):
			sep->config.capabilities.lhdc_v3.sampling_freq = LHDC_SAMPLING_FREQ_44100;
			break;
		case A2DP_CODEC_VENDOR_ID(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID):
			sep->config.capabilities.lhdc_v5.sampling_freq = LHDC_SAMPLING_FREQ_44100;
			break;
		}
	return 0;
}

static int a2dp_lhdc_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_lhdc_enc_thread, "ba-a2dp-lhdc");
}

static int a2dp_lhdc_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_lhdc_dec_thread, "ba-a2dp-lhdc");
}

struct a2dp_sep a2dp_lhdc_v2_source = {
	.name = "A2DP Source (LHDC v2)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID),
		.caps_size = sizeof(a2dp_lhdc_v2_t),
		.capabilities.lhdc_v2 = {
			.info = A2DP_VENDOR_INFO_INIT(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID),
			.sampling_freq =
				LHDC_SAMPLING_FREQ_44100 |
				LHDC_SAMPLING_FREQ_48000 |
				LHDC_SAMPLING_FREQ_96000,
			.bit_depth =
				LHDC_BIT_DEPTH_16 |
				LHDC_BIT_DEPTH_24,
			.max_bitrate = LHDC_MAX_BITRATE_900K,
			.ch_split_mode = LHDC_CH_SPLIT_MODE_NONE,
		},
	},
	.init = a2dp_lhdc_source_init,
	.configuration_select = a2dp_lhdc_v2_configuration_select,
	.configuration_check = a2dp_lhdc_v2_configuration_check,
	.transport_init = a2dp_lhdc_transport_init,
	.transport_start = a2dp_lhdc_source_transport_start,
	.caps_helpers = &a2dp_lhdc_v2_caps_helpers,
};

struct a2dp_sep a2dp_lhdc_v2_sink = {
	.name = "A2DP Sink (LHDC v2)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID),
		.caps_size = sizeof(a2dp_lhdc_v2_t),
		.capabilities.lhdc_v2 = {
			.info = A2DP_VENDOR_INFO_INIT(LHDC_V2_VENDOR_ID, LHDC_V2_CODEC_ID),
			.sampling_freq =
				LHDC_SAMPLING_FREQ_44100 |
				LHDC_SAMPLING_FREQ_48000 |
				LHDC_SAMPLING_FREQ_96000,
			.bit_depth =
				LHDC_BIT_DEPTH_16 |
				LHDC_BIT_DEPTH_24,
			.max_bitrate = LHDC_MAX_BITRATE_900K,
			.ch_split_mode = LHDC_CH_SPLIT_MODE_NONE,
		},
	},
	.configuration_select = a2dp_lhdc_v2_configuration_select,
	.configuration_check = a2dp_lhdc_v2_configuration_check,
	.transport_init = a2dp_lhdc_transport_init,
	.transport_start = a2dp_lhdc_sink_transport_start,
	.caps_helpers = &a2dp_lhdc_v2_caps_helpers,
};

struct a2dp_sep a2dp_lhdc_v3_source = {
	.name = "A2DP Source (LHDC v3)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID),
		.caps_size = sizeof(a2dp_lhdc_v3_t),
		.capabilities.lhdc_v3 = {
			.info = A2DP_VENDOR_INFO_INIT(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID),
			.sampling_freq =
				LHDC_SAMPLING_FREQ_44100 |
				LHDC_SAMPLING_FREQ_48000 |
				LHDC_SAMPLING_FREQ_96000,
			.bit_depth =
				LHDC_BIT_DEPTH_16 |
				LHDC_BIT_DEPTH_24,
			.min_bitrate = 0,
			.max_bitrate = LHDC_MAX_BITRATE_900K,
			.llac = 0, // TODO: copy LLAC/V3/V4 logic from AOSP patches
			.version = LHDC_VER3,
			.lhdc_v4 = 1,
			.ch_split_mode = LHDC_CH_SPLIT_MODE_NONE,
		},
	},
	.init = a2dp_lhdc_source_init,
	.configuration_select = a2dp_lhdc_v3_configuration_select,
	.configuration_check = a2dp_lhdc_v3_configuration_check,
	.transport_init = a2dp_lhdc_transport_init,
	.transport_start = a2dp_lhdc_source_transport_start,
	.caps_helpers = &a2dp_lhdc_v3_caps_helpers,
};

struct a2dp_sep a2dp_lhdc_v3_sink = {
	.name = "A2DP Sink (LHDC v3)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID),
		.caps_size = sizeof(a2dp_lhdc_v3_t),
		.capabilities.lhdc_v3 = {
			.info = A2DP_VENDOR_INFO_INIT(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID),
			.sampling_freq =
				LHDC_SAMPLING_FREQ_44100 |
				LHDC_SAMPLING_FREQ_48000 |
				LHDC_SAMPLING_FREQ_96000,
			.bit_depth =
				LHDC_BIT_DEPTH_16 |
				LHDC_BIT_DEPTH_24,
			.min_bitrate = 0,
			.max_bitrate = LHDC_MAX_BITRATE_900K,
			.llac = 1,
			.version = LHDC_VER3,
			.lhdc_v4 = 1,
			.ch_split_mode = LHDC_CH_SPLIT_MODE_NONE,
		},
	},
	.configuration_select = a2dp_lhdc_v3_configuration_select,
	.configuration_check = a2dp_lhdc_v3_configuration_check,
	.transport_init = a2dp_lhdc_transport_init,
	.transport_start = a2dp_lhdc_sink_transport_start,
	.caps_helpers = &a2dp_lhdc_v3_caps_helpers,
};

struct a2dp_sep a2dp_lhdc_v5_source = {
	.name = "A2DP Source (LHDC v5)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID),
		.caps_size = sizeof(a2dp_lhdc_v5_t),
		.capabilities.lhdc_v5 = {
			.info = A2DP_VENDOR_INFO_INIT(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID),
			.sampling_freq =
				LHDC_SAMPLING_FREQ_44100 |
				LHDC_SAMPLING_FREQ_48000 |
				LHDC_SAMPLING_FREQ_96000,
			.bit_depth =
				LHDC_BIT_DEPTH_16 |
				LHDC_BIT_DEPTH_24,
			.min_bitrate = 0,
			.max_bitrate = LHDC_MAX_BITRATE_900K,
			.version = LHDC_VER3,
		},
	},
	.init = a2dp_lhdc_source_init,
	.configuration_select = a2dp_lhdc_v5_configuration_select,
	.configuration_check = a2dp_lhdc_v5_configuration_check,
	.transport_init = a2dp_lhdc_transport_init,
	.transport_start = a2dp_lhdc_source_transport_start,
	.caps_helpers = &a2dp_lhdc_v5_caps_helpers,
};

struct a2dp_sep a2dp_lhdc_v5_sink = {
	.name = "A2DP Sink (LHDC v5)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID),
		.caps_size = sizeof(a2dp_lhdc_v5_t),
		.capabilities.lhdc_v5 = {
			.info = A2DP_VENDOR_INFO_INIT(LHDC_V5_VENDOR_ID, LHDC_V5_CODEC_ID),
			.sampling_freq =
				LHDC_SAMPLING_FREQ_44100 |
				LHDC_SAMPLING_FREQ_48000 |
				LHDC_SAMPLING_FREQ_96000,
			.bit_depth =
				LHDC_BIT_DEPTH_16 |
				LHDC_BIT_DEPTH_24,
			.min_bitrate = 0,
			.max_bitrate = LHDC_MAX_BITRATE_900K,
			.version = LHDC_VER3,
		},
	},
	.configuration_select = a2dp_lhdc_v5_configuration_select,
	.configuration_check = a2dp_lhdc_v5_configuration_check,
	.transport_init = a2dp_lhdc_transport_init,
	.transport_start = a2dp_lhdc_sink_transport_start,
	.caps_helpers = &a2dp_lhdc_v5_caps_helpers,
};
