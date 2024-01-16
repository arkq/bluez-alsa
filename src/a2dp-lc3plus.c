/*
 * BlueALSA - a2dp-lc3plus.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-lc3plus.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lc3plus.h>

#include "a2dp.h"
#include "audio.h"
#include "ba-transport.h"
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

static bool a2dp_lc3plus_supported(int samplerate, int channels) {

	if (lc3plus_channels_supported(channels) == 0) {
		error("Number of channels not supported by LC3plus library: %u", channels);
		return false;
	}

	if (lc3plus_samplerate_supported(samplerate) == 0) {
		error("Sampling frequency not supported by LC3plus library: %u", samplerate);
		return false;
	}

	return true;
}

static LC3PLUS_Enc *a2dp_lc3plus_enc_init(int samplerate, int channels) {
	LC3PLUS_Enc *handle;
	int32_t lfe_channel_array[1] = { 0 };
	if ((handle = malloc(lc3plus_enc_get_size(samplerate, channels))) != NULL &&
			lc3plus_enc_init(handle, samplerate, channels, 1, lfe_channel_array) == LC3PLUS_OK)
		return handle;
	free(handle);
	return NULL;
}

static void a2dp_lc3plus_enc_free(LC3PLUS_Enc *handle) {
	if (handle == NULL)
		return;
	lc3plus_free_encoder_structs(handle);
	free(handle);
}

static LC3PLUS_Dec *a2dp_lc3plus_dec_init(int samplerate, int channels) {
	LC3PLUS_Dec *handle;
	if ((handle = malloc(lc3plus_dec_get_size(samplerate, channels))) != NULL &&
			lc3plus_dec_init(handle, samplerate, channels, LC3PLUS_PLC_ADVANCED, 1) == LC3PLUS_OK)
		return handle;
	free(handle);
	return NULL;
}

static void a2dp_lc3plus_dec_free(LC3PLUS_Dec *handle) {
	if (handle == NULL)
		return;
	lc3plus_free_decoder_structs(handle);
	free(handle);
}

static int a2dp_lc3plus_get_frame_dms(const a2dp_lc3plus_t *conf) {
	switch (conf->frame_duration) {
	default:
		return 0;
	case LC3PLUS_FRAME_DURATION_025:
		return 25;
	case LC3PLUS_FRAME_DURATION_050:
		return 50;
	case LC3PLUS_FRAME_DURATION_100:
		return 100;
	}
}

void *a2dp_lc3plus_enc_thread(struct ba_transport_pcm *t_pcm) {

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	const a2dp_lc3plus_t *configuration = &t->a2dp.configuration.lc3plus;
	const int lc3plus_frame_dms = a2dp_lc3plus_get_frame_dms(configuration);
	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;
	const unsigned int rtp_ts_clockrate = 96000;

	/* check whether library supports selected configuration */
	if (!a2dp_lc3plus_supported(samplerate, channels))
		goto fail_init;

	LC3PLUS_Enc *handle;
	LC3PLUS_Error err;

	if ((handle = a2dp_lc3plus_enc_init(samplerate, channels)) == NULL) {
		error("Couldn't initialize LC3plus codec: %s", strerror(errno));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(a2dp_lc3plus_enc_free), handle);

	if ((err = lc3plus_enc_set_frame_dms(handle, lc3plus_frame_dms)) != LC3PLUS_OK) {
		error("Couldn't set frame length: %s", lc3plus_strerror(err));
		goto fail_setup;
	}
	if ((err = lc3plus_enc_set_bitrate(handle, config.lc3plus_bitrate)) != LC3PLUS_OK) {
		error("Couldn't set bitrate: %s", lc3plus_strerror(err));
		goto fail_setup;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const size_t lc3plus_ch_samples = lc3plus_enc_get_input_samples(handle);
	const size_t lc3plus_frame_samples = lc3plus_ch_samples * channels;
	const size_t lc3plus_frame_len = lc3plus_enc_get_num_bytes(handle);

	const size_t rtp_headers_len = RTP_HEADER_LEN + sizeof(rtp_media_header_t);
	const size_t mtu_write_payload_len = t->mtu_write - rtp_headers_len;

	size_t ffb_pcm_len = lc3plus_frame_samples;
	if (mtu_write_payload_len / lc3plus_frame_len > 1)
		/* account for possible LC3plus frames packing */
		ffb_pcm_len *= mtu_write_payload_len / lc3plus_frame_len;

	size_t ffb_bt_len = t->mtu_write;
	if (ffb_bt_len < rtp_headers_len + lc3plus_frame_len)
		/* bigger than MTU buffer will be fragmented later */
		ffb_bt_len = rtp_headers_len + lc3plus_frame_len;

	int32_t *pcm_ch1 = malloc(lc3plus_ch_samples * sizeof(int32_t));
	int32_t *pcm_ch2 = malloc(lc3plus_ch_samples * sizeof(int32_t));
	int32_t *pcm_ch_buffers[2] = { pcm_ch1, pcm_ch2 };
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch1);
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch2);

	if (ffb_init_int32_t(&pcm, ffb_pcm_len) == -1 ||
			ffb_init_uint8_t(&bt, ffb_bt_len) == -1 ||
			pcm_ch1 == NULL || pcm_ch2 == NULL) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to the RTP clock rate */
	rtp_state_init(&rtp, samplerate, rtp_ts_clockrate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t samples = ffb_len_in(&pcm);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, pcm.tail, samples)) {
		case -1:
			if (errno == ESTALE) {
				int encoded = 0;
				void *scratch = NULL;
				memset(pcm_ch1, 0, lc3plus_ch_samples * sizeof(*pcm_ch1));
				memset(pcm_ch2, 0, lc3plus_ch_samples * sizeof(*pcm_ch2));
				/* flush encoder internal buffers by feeding it with silence */
				lc3plus_enc24(handle, pcm_ch_buffers, rtp_payload, &encoded, scratch);
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

		/* anchor for RTP payload */
		bt.tail = rtp_payload;

		const int32_t *input = pcm.data;
		size_t input_samples = samples;
		size_t output_len = ffb_len_in(&bt);
		size_t pcm_frames = 0;
		size_t lc3plus_frames = 0;

		/* pack as many LC3plus frames as possible */
		while (input_samples >= lc3plus_frame_samples &&
				output_len >= lc3plus_frame_len &&
				/* RTP packet shall not exceed 20.0 ms of audio */
				lc3plus_frames * lc3plus_frame_dms <= 200 &&
				/* do not overflow RTP frame counter */
				lc3plus_frames < ((1 << 4) - 1)) {

			int encoded = 0;
			void *scratch = NULL;
			audio_deinterleave_s24_4le(input, lc3plus_ch_samples, channels, pcm_ch1, pcm_ch2);
			if ((err = lc3plus_enc24(handle, pcm_ch_buffers, bt.tail, &encoded, scratch)) != LC3PLUS_OK) {
				error("LC3plus encoding error: %s", lc3plus_strerror(err));
				break;
			}

			input += lc3plus_frame_samples;
			input_samples -= lc3plus_frame_samples;
			ffb_seek(&bt, encoded);
			output_len -= encoded;
			pcm_frames += lc3plus_ch_samples;
			lc3plus_frames++;

		}

		if (lc3plus_frames > 0) {

			size_t payload_len_max = t->mtu_write - rtp_headers_len;
			size_t payload_len = ffb_blen_out(&bt) - rtp_headers_len;
			memset(rtp_media_header, 0, sizeof(*rtp_media_header));
			rtp_media_header->frame_count = lc3plus_frames;

			/* If the size of the RTP packet exceeds writing MTU, the RTP payload
			 * should be fragmented. The fragmentation scheme is defined by the
			 * vendor specific LC3plus Bluetooth A2DP specification. */

			if (payload_len > payload_len_max) {
				rtp_media_header->fragmented = 1;
				rtp_media_header->first_fragment = 1;
				rtp_media_header->frame_count = DIV_ROUND_UP(payload_len, payload_len_max);
			}

			for (;;) {

				size_t chunk_len;
				chunk_len = payload_len > payload_len_max ? payload_len_max : payload_len;
				rtp_state_new_frame(&rtp, rtp_header);

				ffb_rewind(&bt);
				ffb_seek(&bt, rtp_headers_len + chunk_len);

				ssize_t len = ffb_blen_out(&bt);
				if ((len = io_bt_write(t_pcm, bt.data, len)) <= 0) {
					if (len == -1)
						error("BT write error: %s", strerror(errno));
					goto fail;
				}

				/* resend RTP headers */
				len -= rtp_headers_len;

				/* break if there is no more payload data */
				if ((payload_len -= len) == 0)
					break;

				/* move the rest of data to the beginning of payload */
				debug("LC3plus payload fragmentation: extra %zu bytes", payload_len);
				memmove(rtp_payload, rtp_payload + len, payload_len);

				rtp_media_header->first_fragment = 0;
				rtp_media_header->last_fragment = payload_len <= payload_len_max;
				rtp_media_header->frame_count--;

			}

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, pcm_frames);

			/* update busy delay (encoding overhead) */
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

			/* If the input buffer was not consumed (due to codesize limit), we
			 * have to append new data to the existing one. Since we do not use
			 * ring buffer, we will simply move unprocessed data to the front
			 * of our linear buffer. */
			ffb_shift(&pcm, samples - input_samples);

		}

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_setup:
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

__attribute__ ((weak))
void *a2dp_lc3plus_dec_thread(struct ba_transport_pcm *t_pcm) {

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	const a2dp_lc3plus_t *configuration = &t->a2dp.configuration.lc3plus;
	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;
	const unsigned int rtp_ts_clockrate = 96000;

	/* check whether library supports selected configuration */
	if (!a2dp_lc3plus_supported(samplerate, channels))
		goto fail_init;

	LC3PLUS_Dec *handle;
	LC3PLUS_Error err;

	if ((handle = a2dp_lc3plus_dec_init(samplerate, channels)) == NULL) {
		error("Couldn't initialize LC3plus codec: %s", strerror(errno));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(a2dp_lc3plus_dec_free), handle);

	const int frame_dms = a2dp_lc3plus_get_frame_dms(configuration);
	if ((err = lc3plus_dec_set_frame_dms(handle, frame_dms)) != LC3PLUS_OK) {
		error("Couldn't set frame length: %s", lc3plus_strerror(err));
		goto fail_setup;
	}

	ffb_t bt = { 0 };
	ffb_t bt_payload = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt_payload);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const size_t lc3plus_ch_samples = lc3plus_dec_get_output_samples(handle);
	const size_t lc3plus_frame_samples = lc3plus_ch_samples * channels;

	int32_t *pcm_ch1 = malloc(lc3plus_ch_samples * sizeof(int32_t));
	int32_t *pcm_ch2 = malloc(lc3plus_ch_samples * sizeof(int32_t));
	int32_t *pcm_ch_buffers[2] = { pcm_ch1, pcm_ch2 };
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch1);
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch2);

	if (ffb_init_int32_t(&pcm, lc3plus_frame_samples) == -1 ||
			ffb_init_uint8_t(&bt_payload, t->mtu_read) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1 ||
			pcm_ch1 == NULL || pcm_ch2 == NULL) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to the RTP clock rate */
	rtp_state_init(&rtp, samplerate, rtp_ts_clockrate);

	/* If true, we should skip fragmented RTP media packets until we will see
	 * not fragmented one or the first fragment of fragmented packet. */
	bool rtp_media_fragment_skip = false;

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, bt.data, len)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		const rtp_header_t *rtp_header = bt.data;
		const rtp_media_header_t *rtp_media_header;
		if ((rtp_media_header = rtp_a2dp_get_payload(rtp_header)) == NULL)
			continue;

		int missing_rtp_frames = 0;
		int missing_pcm_frames = 0;
		rtp_state_sync_stream(&rtp, rtp_header, &missing_rtp_frames, &missing_pcm_frames);

		/* If missing RTP frame was reported and current RTP media frame is marked
		 * as fragmented but it is not the first fragment it means that we are
		 * missing the beginning of it. In such a case we will have to skip the
		 * entire incomplete RTP media frame. */
		if (missing_rtp_frames > 0 &&
				rtp_media_header->fragmented && !rtp_media_header->first_fragment) {
			rtp_media_fragment_skip = true;
			ffb_rewind(&bt_payload);
		}

		if (!ba_transport_pcm_is_active(t_pcm)) {
			rtp.synced = false;
			continue;
		}

#if DEBUG
		if (missing_pcm_frames > 0) {
			size_t missing_lc3plus_frames = DIV_ROUND_UP(missing_pcm_frames, lc3plus_ch_samples);
			debug("Missing LC3plus frames: %zu", missing_lc3plus_frames);
		}
#endif

		while (missing_pcm_frames > 0) {

			void *scratch = NULL;
			lc3plus_dec24(handle, bt_payload.data, 0, pcm_ch_buffers, scratch, 1);
			audio_interleave_s24_4le(pcm_ch1, pcm_ch2, lc3plus_ch_samples, channels, pcm.data);

			warn("Missing LC3plus data, loss concealment applied");

			const size_t samples = lc3plus_frame_samples;
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

			missing_pcm_frames -= lc3plus_ch_samples;

		}

		if (rtp_media_fragment_skip) {
			if (rtp_media_header->fragmented && !rtp_media_header->first_fragment)
				continue;
			rtp_media_fragment_skip = false;
		}

		const uint8_t *payload = (uint8_t *)(rtp_media_header + 1);
		const size_t payload_len = len - (payload - (uint8_t *)bt.data);

		size_t len_;
		if (rtp_media_header->fragmented &&
				rtp_media_header->first_fragment &&
				bt_payload.nmemb < (len_ = rtp_media_header->frame_count * t->mtu_read)) {
			debug("Resizing LC3plus payload buffer: %zd -> %zd", bt_payload.nmemb, len_);
			if (ffb_init_uint8_t(&bt_payload, len_) == -1)
				error("Couldn't resize LC3plus payload buffer: %s", strerror(errno));
		}

		if (ffb_len_in(&bt_payload) >= payload_len) {
			memcpy(bt_payload.tail, payload, payload_len);
			ffb_seek(&bt_payload, payload_len);
		}

		if (rtp_media_header->fragmented &&
				!rtp_media_header->last_fragment) {
			debug("Fragmented LC3plus frame [%u]: payload len: %zd",
					rtp.seq_number, payload_len);
			continue;
		}

		uint8_t *lc3plus_payload = bt_payload.data;
		/* For not-fragmented transfer, the frame count shall indicate the number
		 * of LC3plus frames within a single RTP payload. In case of fragmented
		 * transfer, the last fragment should have the frame count set to 1. */
		size_t lc3plus_frames = rtp_media_header->frame_count;
		size_t lc3plus_frame_len = ffb_blen_out(&bt_payload) / lc3plus_frames;

		/* Decode retrieved LC3plus frames. */
		while (lc3plus_frames--) {

			void *scratch = NULL;
			err = lc3plus_dec24(handle, lc3plus_payload, lc3plus_frame_len, pcm_ch_buffers, scratch, 0);
			audio_interleave_s24_4le(pcm_ch1, pcm_ch2, lc3plus_ch_samples, channels, pcm.data);

			if (err == LC3PLUS_DECODE_ERROR)
				warn("Corrupted LC3plus data, loss concealment applied");
			else if (err != LC3PLUS_OK) {
				error("LC3plus decoding error: %s", lc3plus_strerror(err));
				break;
			}

			lc3plus_payload += lc3plus_frame_len;

			const size_t samples = lc3plus_frame_samples;
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

			/* update local state with decoded PCM frames */
			rtp_state_update(&rtp, lc3plus_ch_samples);

		}

		/* make room for new payload */
		ffb_rewind(&bt_payload);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_setup:
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

static const struct a2dp_channels a2dp_lc3plus_channels[] = {
	{ 1, LC3PLUS_CHANNELS_1 },
	{ 2, LC3PLUS_CHANNELS_2 },
	{ 0 },
};

static const struct a2dp_sampling a2dp_lc3plus_samplings[] = {
	{ 48000, LC3PLUS_SAMPLING_FREQ_48000 },
	{ 96000, LC3PLUS_SAMPLING_FREQ_96000 },
	{ 0 },
};

static int a2dp_lc3plus_configuration_select(
		const struct a2dp_codec *codec,
		void *capabilities) {

	a2dp_lc3plus_t *caps = capabilities;
	const a2dp_lc3plus_t saved = *caps;

	/* narrow capabilities to values supported by BlueALSA */
	if (a2dp_filter_capabilities(codec, &codec->capabilities,
				caps, sizeof(*caps)) != 0)
		return -1;

	if (caps->frame_duration & LC3PLUS_FRAME_DURATION_100)
		caps->frame_duration = LC3PLUS_FRAME_DURATION_100;
	else if (caps->frame_duration & LC3PLUS_FRAME_DURATION_050)
		caps->frame_duration = LC3PLUS_FRAME_DURATION_050;
	else if (caps->frame_duration & LC3PLUS_FRAME_DURATION_025)
		caps->frame_duration = LC3PLUS_FRAME_DURATION_025;
	else {
		error("LC3plus: No supported frame durations: %#x", saved.frame_duration);
		return errno = ENOTSUP, -1;
	}

	const struct a2dp_channels *channels;
	if ((channels = a2dp_channels_select(a2dp_lc3plus_channels, caps->channels)) != NULL)
		caps->channels = channels->value;
	else {
		error("LC3plus: No supported channels: %#x", saved.channels);
		return errno = ENOTSUP, -1;
	}

	const struct a2dp_sampling *sampling;
	const uint16_t caps_frequency = A2DP_LC3PLUS_GET_FREQUENCY(*caps);
	if ((sampling = a2dp_sampling_select(a2dp_lc3plus_samplings, caps_frequency)) != NULL)
		A2DP_LC3PLUS_SET_FREQUENCY(*caps, sampling->value);
	else {
		error("LC3plus: No supported sampling frequencies: %#x", A2DP_LC3PLUS_GET_FREQUENCY(saved));
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_lc3plus_configuration_check(
		const struct a2dp_codec *codec,
		const void *configuration) {

	const a2dp_lc3plus_t *conf = configuration;
	a2dp_lc3plus_t conf_v = *conf;

	/* validate configuration against BlueALSA capabilities */
	if (a2dp_filter_capabilities(codec, &codec->capabilities,
				&conf_v, sizeof(conf_v)) != 0)
		return A2DP_CHECK_ERR_SIZE;

	switch (conf_v.frame_duration) {
	case LC3PLUS_FRAME_DURATION_025:
	case LC3PLUS_FRAME_DURATION_050:
	case LC3PLUS_FRAME_DURATION_100:
		break;
	default:
		debug("LC3plus: Invalid frame duration: %#x", conf->frame_duration);
		return A2DP_CHECK_ERR_FRAME_DURATION;
	}

	if (a2dp_channels_lookup(a2dp_lc3plus_channels, conf_v.channels) == NULL) {
		debug("LC3plus: Invalid channel mode: %#x", conf->channels);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	uint16_t conf_frequency = A2DP_LC3PLUS_GET_FREQUENCY(conf_v);
	if (a2dp_sampling_lookup(a2dp_lc3plus_samplings, conf_frequency) == NULL) {
		debug("LC3plus: Invalid sampling frequency: %#x", A2DP_LC3PLUS_GET_FREQUENCY(*conf));
		return A2DP_CHECK_ERR_SAMPLING;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_lc3plus_transport_init(struct ba_transport *t) {

	const struct a2dp_channels *channels;
	if ((channels = a2dp_channels_lookup(a2dp_lc3plus_channels,
					t->a2dp.configuration.lc3plus.channels)) == NULL)
		return -1;

	const struct a2dp_sampling *sampling;
	if ((sampling = a2dp_sampling_lookup(a2dp_lc3plus_samplings,
					A2DP_LC3PLUS_GET_FREQUENCY(t->a2dp.configuration.lc3plus))) == NULL)
		return -1;

	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S24_4LE;
	t->a2dp.pcm.channels = channels->count;
	t->a2dp.pcm.sampling = sampling->frequency;

	return 0;
}

static int a2dp_lc3plus_source_init(struct a2dp_codec *codec) {
	if (config.a2dp.force_mono)
		codec->capabilities.lc3plus.channels = LC3PLUS_CHANNELS_1;
	if (config.a2dp.force_44100)
		warn("LC3plus: 44.1 kHz sampling frequency not supported");
	return 0;
}

static int a2dp_lc3plus_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_lc3plus_enc_thread, "ba-a2dp-lc3p");
}

struct a2dp_codec a2dp_lc3plus_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_LC3PLUS,
	.synopsis = "A2DP Source (LC3plus)",
	.capabilities.lc3plus = {
		.info = A2DP_VENDOR_INFO_INIT(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
		.frame_duration =
			LC3PLUS_FRAME_DURATION_025 |
			LC3PLUS_FRAME_DURATION_050 |
			LC3PLUS_FRAME_DURATION_100,
		.channels =
			LC3PLUS_CHANNELS_1 |
			LC3PLUS_CHANNELS_2,
		A2DP_LC3PLUS_INIT_FREQUENCY(
				LC3PLUS_SAMPLING_FREQ_48000 |
				LC3PLUS_SAMPLING_FREQ_96000)
	},
	.capabilities_size = sizeof(a2dp_lc3plus_t),
	.init = a2dp_lc3plus_source_init,
	.configuration_select = a2dp_lc3plus_configuration_select,
	.configuration_check = a2dp_lc3plus_configuration_check,
	.transport_init = a2dp_lc3plus_transport_init,
	.transport_start = a2dp_lc3plus_source_transport_start,
};

static int a2dp_lc3plus_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_lc3plus_dec_thread, "ba-a2dp-lc3p");
}

struct a2dp_codec a2dp_lc3plus_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_LC3PLUS,
	.synopsis = "A2DP Sink (LC3plus)",
	.capabilities.lc3plus = {
		.info = A2DP_VENDOR_INFO_INIT(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
		.frame_duration =
			LC3PLUS_FRAME_DURATION_025 |
			LC3PLUS_FRAME_DURATION_050 |
			LC3PLUS_FRAME_DURATION_100,
		.channels =
			LC3PLUS_CHANNELS_1 |
			LC3PLUS_CHANNELS_2,
		A2DP_LC3PLUS_INIT_FREQUENCY(
				LC3PLUS_SAMPLING_FREQ_48000 |
				LC3PLUS_SAMPLING_FREQ_96000)
	},
	.capabilities_size = sizeof(a2dp_lc3plus_t),
	.configuration_select = a2dp_lc3plus_configuration_select,
	.configuration_check = a2dp_lc3plus_configuration_check,
	.transport_init = a2dp_lc3plus_transport_init,
	.transport_start = a2dp_lc3plus_sink_transport_start,
};
