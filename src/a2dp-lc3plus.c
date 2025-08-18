/*
 * BlueALSA - a2dp-lc3plus.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
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

static const struct a2dp_bit_mapping a2dp_lc3plus_channels[] = {
	{ LC3PLUS_CHANNEL_MODE_MONO, .ch = { 1, a2dp_channel_map_mono } },
	{ LC3PLUS_CHANNEL_MODE_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ 0 }
};

static const struct a2dp_bit_mapping a2dp_lc3plus_rates[] = {
	{ LC3PLUS_SAMPLING_FREQ_48000, { 48000 } },
	{ LC3PLUS_SAMPLING_FREQ_96000, { 96000 } },
	{ 0 }
};

static void a2dp_lc3plus_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_lc3plus_t));
}

static int a2dp_lc3plus_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_lc3plus_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_lc3plus_channels, caps->channel_mode, func, userdata);
	return -1;
}

static int a2dp_lc3plus_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_lc3plus_t *caps = capabilities;
	if (stream == A2DP_MAIN) {
		const uint16_t sampling_freq = A2DP_LC3PLUS_GET_SAMPLING_FREQ(*caps);
		return a2dp_bit_mapping_foreach(a2dp_lc3plus_rates, sampling_freq, func, userdata);
	}
	return -1;
}

static void a2dp_lc3plus_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	a2dp_lc3plus_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->channel_mode = a2dp_bit_mapping_lookup_value(a2dp_lc3plus_channels,
				caps->channel_mode, channels);
}

static void a2dp_lc3plus_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_lc3plus_t *caps = capabilities;
	if (stream == A2DP_MAIN) {
		const uint16_t sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_lc3plus_rates,
				A2DP_LC3PLUS_GET_SAMPLING_FREQ(*caps), rate);
		A2DP_LC3PLUS_SET_SAMPLING_FREQ(*caps, sampling_freq);
	}
}

static struct a2dp_caps_helpers a2dp_lc3plus_caps_helpers = {
	.intersect = a2dp_lc3plus_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_lc3plus_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_lc3plus_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_lc3plus_caps_select_channel_mode,
	.select_sample_rate = a2dp_lc3plus_caps_select_sample_rate,
};

static bool a2dp_lc3plus_supported(int rate, int channels) {

	if (lc3plus_channels_supported(channels) == 0) {
		error("Number of channels not supported by LC3plus library: %u", channels);
		return false;
	}

	if (lc3plus_samplerate_supported(rate) == 0) {
		error("sample rate not supported by LC3plus library: %u", rate);
		return false;
	}

	return true;
}

static LC3PLUS_Enc *a2dp_lc3plus_enc_init(int rate, int channels) {
	LC3PLUS_Enc *handle;
	int32_t lfe_channel_array[1] = { 0 };
	if ((handle = malloc(lc3plus_enc_get_size(rate, channels))) != NULL &&
			lc3plus_enc_init(handle, rate, channels, 1, lfe_channel_array) == LC3PLUS_OK)
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

static LC3PLUS_Dec *a2dp_lc3plus_dec_init(int rate, int channels) {
	LC3PLUS_Dec *handle;
	if ((handle = malloc(lc3plus_dec_get_size(rate, channels))) != NULL &&
			lc3plus_dec_init(handle, rate, channels, LC3PLUS_PLC_ADVANCED, 1) == LC3PLUS_OK)
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

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	const a2dp_lc3plus_t *configuration = &t->media.configuration.lc3plus;
	const int lc3plus_frame_dms = a2dp_lc3plus_get_frame_dms(configuration);
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	const unsigned int rtp_ts_clockrate = 96000;

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	/* check whether library supports selected configuration */
	if (!a2dp_lc3plus_supported(rate, channels))
		goto fail_init;

	LC3PLUS_Enc *handle;
	LC3PLUS_Error err;

	if ((handle = a2dp_lc3plus_enc_init(rate, channels)) == NULL) {
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

	const size_t lc3plus_frame_pcm_frames = lc3plus_enc_get_input_samples(handle);
	const size_t lc3plus_frame_pcm_samples = lc3plus_frame_pcm_frames * channels;
	const size_t lc3plus_frame_len = lc3plus_enc_get_num_bytes(handle);

	const size_t rtp_headers_len = RTP_HEADER_LEN + sizeof(rtp_media_header_t);
	const size_t mtu_write_payload_len = t->mtu_write - rtp_headers_len;

	size_t ffb_pcm_len = lc3plus_frame_pcm_samples;
	if (mtu_write_payload_len / lc3plus_frame_len > 1)
		/* account for possible LC3plus frames packing */
		ffb_pcm_len *= mtu_write_payload_len / lc3plus_frame_len;

	size_t ffb_bt_len = t->mtu_write;
	if (ffb_bt_len < rtp_headers_len + lc3plus_frame_len)
		/* bigger than MTU buffer will be fragmented later */
		ffb_bt_len = rtp_headers_len + lc3plus_frame_len;

	int32_t *pcm_ch1 = malloc(lc3plus_frame_pcm_frames * sizeof(int32_t));
	int32_t *pcm_ch2 = malloc(lc3plus_frame_pcm_frames * sizeof(int32_t));
	int32_t *pcm_ch_buffers[2] = { pcm_ch1, pcm_ch2 };
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch1);
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch2);

	if (ffb_init_int32_t(&pcm, ffb_pcm_len) == -1 ||
			ffb_init_uint8_t(&bt, ffb_bt_len) == -1 ||
			pcm_ch1 == NULL || pcm_ch2 == NULL) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	/* Get the total delay introduced by the codec. The LC3plus library
	 * reports total codec delay in case of both encoder and decoder API.
	 * In order not to overestimate the delay, we are not going to report
	 * delay in the decoder thread. */
	const int lc3plus_delay_pcm_frames = lc3plus_enc_get_delay(handle);
	t_pcm->codec_delay_dms = lc3plus_delay_pcm_frames * 10000 / rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to the RTP clock rate */
	rtp_state_init(&rtp, rate, rtp_ts_clockrate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				int encoded = 0;
				void *scratch = NULL;
				memset(pcm_ch1, 0, lc3plus_frame_pcm_frames * sizeof(*pcm_ch1));
				memset(pcm_ch2, 0, lc3plus_frame_pcm_frames * sizeof(*pcm_ch2));
				/* flush encoder internal buffers by feeding it with silence */
				lc3plus_enc24(handle, pcm_ch_buffers, rtp_payload, &encoded, scratch);
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

		const int32_t *input = pcm.data;
		size_t input_samples = ffb_len_out(&pcm);
		size_t output_len = ffb_len_in(&bt);
		size_t pcm_frames = 0;
		size_t lc3plus_frames = 0;

		/* Pack as many LC3plus frames as possible. */
		while (input_samples >= lc3plus_frame_pcm_samples &&
				output_len >= lc3plus_frame_len &&
				/* RTP packet shall not exceed 20.0 ms of audio */
				lc3plus_frames * lc3plus_frame_dms <= 200 &&
				/* do not overflow RTP frame counter */
				lc3plus_frames < ((1 << 4) - 1)) {

			int encoded = 0;
			void *scratch = NULL;
			audio_deinterleave_s24_4le(pcm_ch_buffers, input, channels, lc3plus_frame_pcm_frames);
			if ((err = lc3plus_enc24(handle, pcm_ch_buffers, bt.tail, &encoded, scratch)) != LC3PLUS_OK) {
				error("LC3plus encoding error: %s", lc3plus_strerror(err));
				break;
			}

			input += lc3plus_frame_pcm_samples;
			input_samples -= lc3plus_frame_pcm_samples;
			ffb_seek(&bt, encoded);
			output_len -= encoded;
			pcm_frames += lc3plus_frame_pcm_frames;
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

				if (!io.initiated) {
					/* Get the delay due to codec processing. */
					t_pcm->processing_delay_dms = asrsync_get_dms_since_last_sync(&io.asrs);
					ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);
					io.initiated = true;
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

			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, pcm_frames);

			/* If the input buffer was not consumed (due to codesize limit), we
			 * have to append new data to the existing one. Since we do not use
			 * ring buffer, we will simply move unprocessed data to the front
			 * of our linear buffer. */
			ffb_shift(&pcm, pcm_frames * channels);

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

	const a2dp_lc3plus_t *configuration = &t->media.configuration.lc3plus;
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	const unsigned int rtp_ts_clockrate = 96000;

	/* check whether library supports selected configuration */
	if (!a2dp_lc3plus_supported(rate, channels))
		goto fail_init;

	LC3PLUS_Dec *handle;
	LC3PLUS_Error err;

	if ((handle = a2dp_lc3plus_dec_init(rate, channels)) == NULL) {
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

	const size_t lc3plus_frame_pcm_frames = lc3plus_dec_get_output_samples(handle);
	const size_t lc3plus_frame_pcm_samples = lc3plus_frame_pcm_frames * channels;

	int32_t *pcm_ch1 = malloc(lc3plus_frame_pcm_frames * sizeof(int32_t));
	int32_t *pcm_ch2 = malloc(lc3plus_frame_pcm_frames * sizeof(int32_t));
	int32_t *pcm_ch_buffers[2] = { pcm_ch1, pcm_ch2 };
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch1);
	pthread_cleanup_push(PTHREAD_CLEANUP(free), pcm_ch2);

	if (ffb_init_int32_t(&pcm, lc3plus_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt_payload, t->mtu_read) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1 ||
			pcm_ch1 == NULL || pcm_ch2 == NULL) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to the RTP clock rate */
	rtp_state_init(&rtp, rate, rtp_ts_clockrate);

	/* If true, we should skip fragmented RTP media packets until we will see
	 * not fragmented one or the first fragment of fragmented packet. */
	bool rtp_media_fragment_skip = false;

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
			size_t missing_lc3plus_frames = DIV_ROUND_UP(missing_pcm_frames, lc3plus_frame_pcm_frames);
			debug("Missing LC3plus frames: %zu", missing_lc3plus_frames);
		}
#endif

		while (missing_pcm_frames > 0) {

			void *scratch = NULL;
			lc3plus_dec24(handle, bt_payload.data, 0, pcm_ch_buffers, scratch, 1);
			audio_interleave_s24_4le(pcm.data, (const int32_t **)pcm_ch_buffers,
					channels, lc3plus_frame_pcm_frames);

			warn("Missing LC3plus data, loss concealment applied");

			const size_t samples = lc3plus_frame_pcm_samples;
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("PCM write error: %s", strerror(errno));

			missing_pcm_frames -= lc3plus_frame_pcm_frames;

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
			audio_interleave_s24_4le(pcm.data, (const int32_t **)pcm_ch_buffers,
					channels, lc3plus_frame_pcm_frames);

			if (err == LC3PLUS_DECODE_ERROR)
				warn("Corrupted LC3plus data, loss concealment applied");
			else if (err != LC3PLUS_OK) {
				error("LC3plus decoding error: %s", lc3plus_strerror(err));
				break;
			}

			lc3plus_payload += lc3plus_frame_len;

			const size_t samples = lc3plus_frame_pcm_samples;
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("PCM write error: %s", strerror(errno));

			/* Update local state with decoded PCM frames. */
			rtp_state_update(&rtp, lc3plus_frame_pcm_frames);

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

static int a2dp_lc3plus_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_lc3plus_t *caps = capabilities;
	const a2dp_lc3plus_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_lc3plus_caps_intersect(caps, &sep->config.capabilities);

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

	unsigned int channel_mode = 0;
	if (a2dp_lc3plus_caps_foreach_channel_mode(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode) != -1)
		caps->channel_mode = channel_mode;
	else {
		error("LC3plus: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	unsigned int sampling_freq = 0;
	if (a2dp_lc3plus_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		A2DP_LC3PLUS_SET_SAMPLING_FREQ(*caps, sampling_freq);
	else {
		error("LC3plus: No supported sample rates: %#x", A2DP_LC3PLUS_GET_SAMPLING_FREQ(saved));
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_lc3plus_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_lc3plus_t *conf = configuration;
	a2dp_lc3plus_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_lc3plus_caps_intersect(&conf_v, &sep->config.capabilities);

	switch (conf_v.frame_duration) {
	case LC3PLUS_FRAME_DURATION_025:
	case LC3PLUS_FRAME_DURATION_050:
	case LC3PLUS_FRAME_DURATION_100:
		break;
	default:
		debug("LC3plus: Invalid frame duration: %#x", conf->frame_duration);
		return A2DP_CHECK_ERR_FRAME_DURATION;
	}

	if (a2dp_bit_mapping_lookup(a2dp_lc3plus_channels, conf_v.channel_mode) == -1) {
		debug("LC3plus: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	uint16_t conf_sampling_freq = A2DP_LC3PLUS_GET_SAMPLING_FREQ(conf_v);
	if (a2dp_bit_mapping_lookup(a2dp_lc3plus_rates, conf_sampling_freq) == -1) {
		debug("LC3plus: Invalid sample rate: %#x", A2DP_LC3PLUS_GET_SAMPLING_FREQ(*conf));
		return A2DP_CHECK_ERR_RATE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_lc3plus_transport_init(struct ba_transport *t) {

	ssize_t channels_i;
	if ((channels_i = a2dp_bit_mapping_lookup(a2dp_lc3plus_channels,
					t->media.configuration.lc3plus.channel_mode)) == -1)
		return -1;

	ssize_t rate_i;
	if ((rate_i = a2dp_bit_mapping_lookup(a2dp_lc3plus_rates,
					A2DP_LC3PLUS_GET_SAMPLING_FREQ(t->media.configuration.lc3plus))) == -1)
		return -1;

	t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S24_4LE;
	t->media.pcm.channels = a2dp_lc3plus_channels[channels_i].value;
	t->media.pcm.rate = a2dp_lc3plus_rates[rate_i].value;

	memcpy(t->media.pcm.channel_map, a2dp_lc3plus_channels[channels_i].ch.map,
			t->media.pcm.channels * sizeof(*t->media.pcm.channel_map));

	return 0;
}

static int a2dp_lc3plus_source_init(struct a2dp_sep *sep) {
	if (config.a2dp.force_mono)
		sep->config.capabilities.lc3plus.channel_mode = LC3PLUS_CHANNEL_MODE_MONO;
	if (config.a2dp.force_44100)
		warn("LC3plus: 44.1 kHz sample rate not supported");
	return 0;
}

static int a2dp_lc3plus_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_lc3plus_enc_thread, "ba-a2dp-lc3p");
}

struct a2dp_sep a2dp_lc3plus_source = {
	.name = "A2DP Source (LC3plus)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
		.caps_size = sizeof(a2dp_lc3plus_t),
		.capabilities.lc3plus = {
			.info = A2DP_VENDOR_INFO_INIT(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
			.frame_duration =
				LC3PLUS_FRAME_DURATION_025 |
				LC3PLUS_FRAME_DURATION_050 |
				LC3PLUS_FRAME_DURATION_100,
			.channel_mode =
				LC3PLUS_CHANNEL_MODE_MONO |
				LC3PLUS_CHANNEL_MODE_STEREO,
			A2DP_LC3PLUS_INIT_SAMPLING_FREQ(
					LC3PLUS_SAMPLING_FREQ_48000 |
					LC3PLUS_SAMPLING_FREQ_96000)
		},
	},
	.init = a2dp_lc3plus_source_init,
	.configuration_select = a2dp_lc3plus_configuration_select,
	.configuration_check = a2dp_lc3plus_configuration_check,
	.transport_init = a2dp_lc3plus_transport_init,
	.transport_start = a2dp_lc3plus_source_transport_start,
	.caps_helpers = &a2dp_lc3plus_caps_helpers,
};

static int a2dp_lc3plus_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_lc3plus_dec_thread, "ba-a2dp-lc3p");
}

struct a2dp_sep a2dp_lc3plus_sink = {
	.name = "A2DP Sink (LC3plus)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
		.caps_size = sizeof(a2dp_lc3plus_t),
		.capabilities.lc3plus = {
			.info = A2DP_VENDOR_INFO_INIT(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
			.frame_duration =
				LC3PLUS_FRAME_DURATION_025 |
				LC3PLUS_FRAME_DURATION_050 |
				LC3PLUS_FRAME_DURATION_100,
			.channel_mode =
				LC3PLUS_CHANNEL_MODE_MONO |
				LC3PLUS_CHANNEL_MODE_STEREO,
			A2DP_LC3PLUS_INIT_SAMPLING_FREQ(
					LC3PLUS_SAMPLING_FREQ_48000 |
					LC3PLUS_SAMPLING_FREQ_96000)
		},
	},
	.configuration_select = a2dp_lc3plus_configuration_select,
	.configuration_check = a2dp_lc3plus_configuration_check,
	.transport_init = a2dp_lc3plus_transport_init,
	.transport_start = a2dp_lc3plus_sink_transport_start,
	.caps_helpers = &a2dp_lc3plus_caps_helpers,
};
