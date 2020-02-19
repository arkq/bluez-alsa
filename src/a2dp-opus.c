/*
 * BlueALSA - a2dp-opus.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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
#include "io.h"
#include "rtp.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

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

	const a2dp_opus_t *configuration = &t->a2dp.configuration.opus;
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int sampling = t->a2dp.pcm.sampling;
	const unsigned int opus_frame_dms = a2dp_opus_get_frame_dms(configuration);
	const size_t opus_frame_pcm_samples = opus_frame_dms * sampling / 10000;
	const size_t opus_frame_pcm_frames = opus_frame_pcm_samples / channels;

	int err;
	if ((opus = opus_encoder_create(sampling, channels, OPUS_APPLICATION_AUDIO, &err)) == NULL ||
			(err = opus_encoder_init(opus, sampling, channels, OPUS_APPLICATION_AUDIO)) != OPUS_OK) {
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

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio sampling rate */
	rtp_state_init(&rtp, sampling, sampling);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				opus_encoder_init(opus, sampling, channels, OPUS_APPLICATION_AUDIO);
				ffb_rewind(&pcm);
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

		/* encode and transfer obtained data */
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

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, opus_frame_pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, opus_frame_pcm_frames);

			/* update busy delay (encoding overhead) */
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

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

	const a2dp_opus_t *configuration = &t->a2dp.configuration.opus;
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int sampling = t->a2dp.pcm.sampling;
	const unsigned int opus_frame_dms = a2dp_opus_get_frame_dms(configuration);
	const size_t opus_frame_pcm_samples = opus_frame_dms * sampling / 10000;

	int err;
	if ((opus = opus_decoder_create(sampling, channels, &err)) == NULL ||
			(err = opus_decoder_init(opus, sampling, channels)) != OPUS_OK) {
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

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio sampling rate */
	rtp_state_init(&rtp, sampling, sampling);

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

static const struct a2dp_channels a2dp_opus_channels[] = {
	{ 1, OPUS_CHANNEL_MODE_MONO },
	{ 2, OPUS_CHANNEL_MODE_STEREO },
	{ 0 },
};

static const struct a2dp_sampling a2dp_opus_samplings[] = {
	{ 48000, OPUS_SAMPLING_FREQ_48000 },
	{ 0 },
};

static int a2dp_opus_configuration_select(
		const struct a2dp_codec *codec,
		void *capabilities) {

	a2dp_opus_t *caps = capabilities;
	const a2dp_opus_t saved = *caps;

	/* narrow capabilities to values supported by BlueALSA */
	if (a2dp_filter_capabilities(codec, &codec->capabilities,
				caps, sizeof(*caps)) != 0)
		return -1;

	const struct a2dp_sampling *sampling;
	if ((sampling = a2dp_sampling_select(a2dp_opus_samplings, caps->frequency)) != NULL)
		caps->frequency = sampling->value;
	else {
		error("Opus: No supported sampling frequencies: %#x", saved.frequency);
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

	const struct a2dp_channels *channels;
	if ((channels = a2dp_channels_select(a2dp_opus_channels, caps->channel_mode)) != NULL)
		caps->channel_mode = channels->value;
	else {
		error("Opus: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_opus_configuration_check(
		const struct a2dp_codec *codec,
		const void *configuration) {

	const a2dp_opus_t *conf = configuration;
	a2dp_opus_t conf_v = *conf;

	/* validate configuration against BlueALSA capabilities */
	if (a2dp_filter_capabilities(codec, &codec->capabilities,
				&conf_v, sizeof(conf_v)) != 0)
		return A2DP_CHECK_ERR_SIZE;

	if (a2dp_sampling_lookup(a2dp_opus_samplings, conf_v.frequency) == NULL) {
		debug("Opus: Invalid sampling frequency: %#x", conf->frequency);
		return A2DP_CHECK_ERR_SAMPLING;
	}

	switch (conf_v.frame_duration) {
	case OPUS_FRAME_DURATION_100:
	case OPUS_FRAME_DURATION_200:
		break;
	default:
		debug("Opus: Invalid frame duration: %#x", conf->frame_duration);
		return A2DP_CHECK_ERR_FRAME_DURATION;
	}

	if (a2dp_channels_lookup(a2dp_opus_channels, conf_v.channel_mode) == NULL) {
		debug("Opus: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_opus_transport_init(struct ba_transport *t) {

	const struct a2dp_channels *channels;
	if ((channels = a2dp_channels_lookup(a2dp_opus_channels,
					t->a2dp.configuration.opus.channel_mode)) == NULL)
		return -1;

	const struct a2dp_sampling *sampling;
	if ((sampling = a2dp_sampling_lookup(a2dp_opus_samplings,
					t->a2dp.configuration.opus.frequency)) == NULL)
		return -1;

	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->a2dp.pcm.channels = channels->count;
	t->a2dp.pcm.sampling = sampling->frequency;

	return 0;
}

static int a2dp_opus_source_init(struct a2dp_codec *codec) {
	if (config.a2dp.force_mono)
		codec->capabilities.opus.channel_mode = OPUS_CHANNEL_MODE_MONO;
	return 0;
}

static int a2dp_opus_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_opus_enc_thread, "ba-a2dp-opus");
}

struct a2dp_codec a2dp_opus_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_OPUS,
	.synopsis = "A2DP Source (Opus)",
	.capabilities.opus = {
		.info = A2DP_VENDOR_INFO_INIT(OPUS_VENDOR_ID, OPUS_CODEC_ID),
		.frequency =
			OPUS_SAMPLING_FREQ_48000,
		.frame_duration =
			OPUS_FRAME_DURATION_100 |
			OPUS_FRAME_DURATION_200,
		.channel_mode =
			OPUS_CHANNEL_MODE_MONO |
			OPUS_CHANNEL_MODE_STEREO,
	},
	.capabilities_size = sizeof(a2dp_opus_t),
	.init = a2dp_opus_source_init,
	.configuration_select = a2dp_opus_configuration_select,
	.configuration_check = a2dp_opus_configuration_check,
	.transport_init = a2dp_opus_transport_init,
	.transport_start = a2dp_opus_source_transport_start,
};

static int a2dp_opus_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_opus_dec_thread, "ba-a2dp-opus");
}

struct a2dp_codec a2dp_opus_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_OPUS,
	.synopsis = "A2DP Sink (Opus)",
	.capabilities.opus = {
		.info = A2DP_VENDOR_INFO_INIT(OPUS_VENDOR_ID, OPUS_CODEC_ID),
		.frequency =
			OPUS_SAMPLING_FREQ_48000,
		.frame_duration =
			OPUS_FRAME_DURATION_100 |
			OPUS_FRAME_DURATION_200,
		.channel_mode =
			OPUS_CHANNEL_MODE_MONO |
			OPUS_CHANNEL_MODE_STEREO,
	},
	.capabilities_size = sizeof(a2dp_opus_t),
	.configuration_select = a2dp_opus_configuration_select,
	.configuration_check = a2dp_opus_configuration_check,
	.transport_init = a2dp_opus_transport_init,
	.transport_start = a2dp_opus_sink_transport_start,
};
