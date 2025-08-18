/*
 * BlueALSA - a2dp-opus.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-opus.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <opus.h>

#include "a2dp.h"
#include "ba-config.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "io.h"
#include "rtp.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_bit_mapping a2dp_opus_channels[] = {
	{ OPUS_CHANNEL_MODE_MONO, .ch = { 1, a2dp_channel_map_mono } },
	{ OPUS_CHANNEL_MODE_DUAL, .ch = { 2, a2dp_channel_map_stereo } },
	{ OPUS_CHANNEL_MODE_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ 0 }
};

static const struct a2dp_bit_mapping a2dp_opus_rates[] = {
	{ OPUS_SAMPLING_FREQ_16000, { 16000 } },
	{ OPUS_SAMPLING_FREQ_24000, { 24000 } },
	{ OPUS_SAMPLING_FREQ_48000, { 48000 } },
	{ 0 }
};

static void a2dp_opus_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_opus_t));
}

static int a2dp_opus_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_opus_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_opus_channels, caps->channel_mode, func, userdata);
	return -1;
}

static int a2dp_opus_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_opus_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_opus_rates, caps->sampling_freq, func, userdata);
	return -1;
}

static void a2dp_opus_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	a2dp_opus_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->channel_mode = a2dp_bit_mapping_lookup_value(a2dp_opus_channels,
				caps->channel_mode, channels);
}

static void a2dp_opus_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_opus_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_opus_rates,
				caps->sampling_freq, rate);
}

static struct a2dp_caps_helpers a2dp_opus_caps_helpers = {
	.intersect = a2dp_opus_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_opus_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_opus_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_opus_caps_select_channel_mode,
	.select_sample_rate = a2dp_opus_caps_select_sample_rate,
};

static unsigned int a2dp_opus_get_frame_dms(const a2dp_opus_t *conf) {
	switch (conf->frame_duration) {
	default:
		return 0;
	case OPUS_FRAME_DURATION_100:
		return 100;
	case OPUS_FRAME_DURATION_200:
		return 200;
	}
}

static void opus_encoder_destroy_ptr(OpusEncoder **p_st) {
	opus_encoder_destroy(*p_st);
}

void *a2dp_opus_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	OpusEncoder *opus = NULL;
	pthread_cleanup_push(PTHREAD_CLEANUP(opus_encoder_destroy_ptr), &opus);

	const a2dp_opus_t *configuration = &t->media.configuration.opus;
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	const unsigned int opus_frame_dms = a2dp_opus_get_frame_dms(configuration);
	const size_t opus_frame_pcm_samples = opus_frame_dms * rate / 10000;
	const size_t opus_frame_pcm_frames = opus_frame_pcm_samples / channels;

	int err;
	if ((opus = opus_encoder_create(rate, channels, OPUS_APPLICATION_AUDIO, &err)) == NULL ||
			(err = opus_encoder_init(opus, rate, channels, OPUS_APPLICATION_AUDIO)) != OPUS_OK) {
		error("Couldn't initialize Opus encoder: %s", opus_strerror(err));
		goto fail_init;
	}

	if ((err = opus_encoder_ctl(opus, OPUS_SET_COMPLEXITY(5))) != OPUS_OK) {
		error("Couldn't set computational complexity: %s", opus_strerror(err));
		goto fail_init;
	}

	if ((err = opus_encoder_ctl(opus, OPUS_SET_BITRATE(128000 * channels))) != OPUS_OK) {
		error("Couldn't set bitrate: %s", opus_strerror(err));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int16_t(&pcm, opus_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	int32_t opus_delay_pcm_frames = 0;
	/* Get the delay introduced by the encoder. */
	opus_encoder_ctl(opus, OPUS_GET_LOOKAHEAD(&opus_delay_pcm_frames));
	t_pcm->codec_delay_dms = opus_delay_pcm_frames * 10000 / rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to PCM sample rate */
	rtp_state_init(&rtp, rate, rate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				opus_encoder_init(opus, rate, channels, OPUS_APPLICATION_AUDIO);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		/* anchor for RTP payload */
		bt.tail = rtp_payload;

		const int16_t *input = pcm.data;
		size_t input_samples = ffb_len_out(&pcm);

		/* Encode and transfer obtained data. */
		while (input_samples >= opus_frame_pcm_samples) {

			ssize_t len;
			if ((len = opus_encode(opus, input, opus_frame_pcm_frames,
							bt.tail, ffb_len_in(&bt))) < 0) {
				error("Opus encoding error: %s", opus_strerror(len));
				break;
			}

			input += opus_frame_pcm_samples;
			input_samples -= opus_frame_pcm_samples;
			ffb_seek(&bt, len);

			rtp_state_new_frame(&rtp, rtp_header);
			rtp_media_header->frame_count = 1;

			len = ffb_blen_out(&bt);
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

			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, opus_frame_pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, opus_frame_pcm_frames);

			/* If the input buffer was not consumed (due to encoder frame
			 * constraint), we have to append new data to the existing one.
			 * Since we do not use ring buffer, we will simply move data
			 * to the front of our linear buffer. */
			ffb_shift(&pcm, opus_frame_pcm_samples);

		}

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static void opus_decoder_destroy_ptr(OpusDecoder **p_st) {
	opus_decoder_destroy(*p_st);
}

__attribute__ ((weak))
void *a2dp_opus_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	OpusDecoder *opus = NULL;
	pthread_cleanup_push(PTHREAD_CLEANUP(opus_decoder_destroy_ptr), &opus);

	const a2dp_opus_t *configuration = &t->media.configuration.opus;
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	const unsigned int opus_frame_dms = a2dp_opus_get_frame_dms(configuration);
	const size_t opus_frame_pcm_samples = opus_frame_dms * rate / 10000;

	int err;
	if ((opus = opus_decoder_create(rate, channels, &err)) == NULL ||
			(err = opus_decoder_init(opus, rate, channels)) != OPUS_OK) {
		error("Couldn't initialize Opus decoder: %s", opus_strerror(err));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int16_t(&pcm, opus_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	int32_t opus_delay_pcm_frames = 0;
	/* Get the delay introduced by the decoder. */
	opus_decoder_ctl(opus, OPUS_GET_LOOKAHEAD(&opus_delay_pcm_frames));
	t_pcm->codec_delay_dms = opus_delay_pcm_frames * 10000 / rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

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
		const rtp_media_header_t *rtp_media_header;
		if ((rtp_media_header = rtp_a2dp_get_payload(rtp_header)) == NULL)
			continue;

		int missing_rtp_frames = 0;
		rtp_state_sync_stream(&rtp, rtp_header, &missing_rtp_frames, NULL);

		if (!ba_transport_pcm_is_active(t_pcm)) {
			rtp.synced = false;
			continue;
		}

		const uint8_t *rtp_payload = (uint8_t *)(rtp_media_header + 1);
		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		if ((len = opus_decode(opus, rtp_payload, rtp_payload_len,
						pcm.data, ffb_blen_in(&pcm), 0)) < 0) {
			error("Opus decoding error: %s", opus_strerror(len));
			break;
		}

		const size_t samples = len * channels;
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
fail_init:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static int a2dp_opus_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_opus_t *caps = capabilities;
	const a2dp_opus_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_opus_caps_intersect(caps, &sep->config.capabilities);

	unsigned int sampling_freq = 0;
	if (a2dp_opus_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		caps->sampling_freq = sampling_freq;
	else {
		error("Opus: No supported sample rates: %#x", saved.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	if (caps->frame_duration & OPUS_FRAME_DURATION_200)
		caps->frame_duration = OPUS_FRAME_DURATION_200;
	else if (caps->frame_duration & OPUS_FRAME_DURATION_100)
		caps->frame_duration = OPUS_FRAME_DURATION_100;
	else {
		error("Opus: No supported frame durations: %#x", saved.frame_duration);
		return errno = ENOTSUP, -1;
	}

	unsigned int channel_mode = 0;
	if (a2dp_opus_caps_foreach_channel_mode(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode) != -1)
		caps->channel_mode = channel_mode;
	else {
		error("Opus: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_opus_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_opus_t *conf = configuration;
	a2dp_opus_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_opus_caps_intersect(&conf_v, &sep->config.capabilities);

	if (a2dp_bit_mapping_lookup(a2dp_opus_rates, conf_v.sampling_freq) == -1) {
		debug("Opus: Invalid sample rate: %#x", conf->sampling_freq);
		return A2DP_CHECK_ERR_RATE;
	}

	switch (conf_v.frame_duration) {
	case OPUS_FRAME_DURATION_100:
	case OPUS_FRAME_DURATION_200:
		break;
	default:
		debug("Opus: Invalid frame duration: %#x", conf->frame_duration);
		return A2DP_CHECK_ERR_FRAME_DURATION;
	}

	if (a2dp_bit_mapping_lookup(a2dp_opus_channels, conf_v.channel_mode) == -1) {
		debug("Opus: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_opus_transport_init(struct ba_transport *t) {

	ssize_t channels_i;
	if ((channels_i = a2dp_bit_mapping_lookup(a2dp_opus_channels,
					t->media.configuration.opus.channel_mode)) == -1)
		return -1;

	ssize_t rate_i;
	if ((rate_i = a2dp_bit_mapping_lookup(a2dp_opus_rates,
					t->media.configuration.opus.sampling_freq)) == -1)
		return -1;

	t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->media.pcm.channels = a2dp_opus_channels[channels_i].value;
	t->media.pcm.rate = a2dp_opus_rates[rate_i].value;

	memcpy(t->media.pcm.channel_map, a2dp_opus_channels[channels_i].ch.map,
			t->media.pcm.channels * sizeof(*t->media.pcm.channel_map));

	return 0;
}

static int a2dp_opus_source_init(struct a2dp_sep *sep) {
	if (config.a2dp.force_mono)
		sep->config.capabilities.opus.channel_mode = OPUS_CHANNEL_MODE_MONO;
	return 0;
}

static int a2dp_opus_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_opus_enc_thread, "ba-a2dp-opus");
}

struct a2dp_sep a2dp_opus_source = {
	.name = "A2DP Source (Opus)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(OPUS_VENDOR_ID, OPUS_CODEC_ID),
		.caps_size = sizeof(a2dp_opus_t),
		.capabilities.opus = {
			.info = A2DP_VENDOR_INFO_INIT(OPUS_VENDOR_ID, OPUS_CODEC_ID),
			.sampling_freq =
				OPUS_SAMPLING_FREQ_48000 |
				OPUS_SAMPLING_FREQ_24000 |
				OPUS_SAMPLING_FREQ_16000,
			.frame_duration =
				OPUS_FRAME_DURATION_100 |
				OPUS_FRAME_DURATION_200,
			.channel_mode =
				OPUS_CHANNEL_MODE_MONO |
				OPUS_CHANNEL_MODE_STEREO,
		},
	},
	.init = a2dp_opus_source_init,
	.configuration_select = a2dp_opus_configuration_select,
	.configuration_check = a2dp_opus_configuration_check,
	.transport_init = a2dp_opus_transport_init,
	.transport_start = a2dp_opus_source_transport_start,
	.caps_helpers = &a2dp_opus_caps_helpers,
};

static int a2dp_opus_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_opus_dec_thread, "ba-a2dp-opus");
}

struct a2dp_sep a2dp_opus_sink = {
	.name = "A2DP Sink (Opus)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(OPUS_VENDOR_ID, OPUS_CODEC_ID),
		.caps_size = sizeof(a2dp_opus_t),
		.capabilities.opus = {
			.info = A2DP_VENDOR_INFO_INIT(OPUS_VENDOR_ID, OPUS_CODEC_ID),
			.sampling_freq =
				OPUS_SAMPLING_FREQ_48000 |
				OPUS_SAMPLING_FREQ_24000 |
				OPUS_SAMPLING_FREQ_16000,
			.frame_duration =
				OPUS_FRAME_DURATION_100 |
				OPUS_FRAME_DURATION_200,
			.channel_mode =
				OPUS_CHANNEL_MODE_MONO |
				OPUS_CHANNEL_MODE_STEREO,
		},
	},
	.configuration_select = a2dp_opus_configuration_select,
	.configuration_check = a2dp_opus_configuration_check,
	.transport_init = a2dp_opus_transport_init,
	.transport_start = a2dp_opus_sink_transport_start,
	.caps_helpers = &a2dp_opus_caps_helpers,
};
