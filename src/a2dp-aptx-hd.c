/*
 * BlueALSA - a2dp-aptx-hd.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-aptx-hd.h"

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

#include "a2dp.h"
#include "ba-config.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "codec-aptx.h"
#include "io.h"
#include "rtp.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_bit_mapping a2dp_aptx_channels[] = {
	{ APTX_CHANNEL_MODE_MONO, .ch = { 1, a2dp_channel_map_mono } },
	{ APTX_CHANNEL_MODE_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ 0 }
};

static const struct a2dp_bit_mapping a2dp_aptx_rates[] = {
	{ APTX_SAMPLING_FREQ_16000, { 16000 } },
	{ APTX_SAMPLING_FREQ_32000, { 32000 } },
	{ APTX_SAMPLING_FREQ_44100, { 44100 } },
	{ APTX_SAMPLING_FREQ_48000, { 48000 } },
	{ 0 }
};

static void a2dp_aptx_hd_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_aptx_hd_t));
}

static int a2dp_aptx_hd_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_aptx_hd_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_aptx_channels, caps->aptx.channel_mode, func, userdata);
	return -1;
}

static int a2dp_aptx_hd_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_aptx_hd_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_aptx_rates, caps->aptx.sampling_freq, func, userdata);
	return -1;
}

static void a2dp_aptx_hd_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	a2dp_aptx_hd_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->aptx.channel_mode = a2dp_bit_mapping_lookup_value(a2dp_aptx_channels,
				caps->aptx.channel_mode, channels);
}

static void a2dp_aptx_hd_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_aptx_hd_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->aptx.sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_aptx_rates,
				caps->aptx.sampling_freq, rate);
}

static struct a2dp_caps_helpers a2dp_aptx_hd_caps_helpers = {
	.intersect = a2dp_aptx_hd_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_aptx_hd_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_aptx_hd_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_aptx_hd_caps_select_channel_mode,
	.select_sample_rate = a2dp_aptx_hd_caps_select_sample_rate,
};

void *a2dp_aptx_hd_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_APTX handle;
	if ((handle = aptxhdenc_init()) == NULL) {
		error("Couldn't initialize apt-X HD encoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(aptxhdenc_destroy), handle);

	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	const size_t aptx_frame_len = 2 * 3 * sizeof(uint8_t);
	const size_t aptx_frame_pcm_samples = 4 * channels;

	const size_t mtu_write_aptx_frames = (t->mtu_write - RTP_HEADER_LEN) / aptx_frame_len;
	if (ffb_init_int32_t(&pcm, aptx_frame_pcm_samples * mtu_write_aptx_frames) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	/* initialize RTP header and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header, NULL, 0);

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to PCM sample rate */
	rtp_state_init(&rtp, rate, rate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE)
				continue;
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		const int32_t *input = pcm.data;
		const size_t samples = ffb_len_out(&pcm);
		size_t input_samples = samples;

		/* Encode and transfer obtained data. */
		while (input_samples >= aptx_frame_pcm_samples) {

			/* anchor for RTP payload */
			bt.tail = rtp_payload;

			size_t output_len = ffb_len_in(&bt);
			size_t pcm_samples = 0;

			/* Generate as many apt-X frames as possible to fill the output buffer
			 * without overflowing it. The size of the output buffer is based on
			 * the socket MTU, so such a transfer should be most efficient. */
			while (input_samples >= aptx_frame_pcm_samples && output_len >= aptx_frame_len) {

				size_t encoded = output_len;
				ssize_t len;

				if ((len = aptxhdenc_encode(handle, input, input_samples, bt.tail, &encoded)) <= 0) {
					error("Apt-X HD encoding error: %s", strerror(errno));
					break;
				}

				input += len;
				input_samples -= len;
				ffb_seek(&bt, encoded);
				output_len -= encoded;
				pcm_samples += len;

			}

			rtp_state_new_frame(&rtp, rtp_header);

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

			const size_t pcm_frames = pcm_samples / channels;
			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp.ts_pcm_frames += pcm_frames;

			/* reinitialize output buffer */
			ffb_rewind(&bt);

		}

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_samples);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

#if HAVE_APTX_HD_DECODE
__attribute__ ((weak))
void *a2dp_aptx_hd_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_APTX handle;
	if ((handle = aptxhddec_init()) == NULL) {
		error("Couldn't initialize apt-X HD decoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(aptxhddec_destroy), handle);

	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	/* Note, that we are allocating space for one extra output packed, which is
	 * required by the aptx_decode_sync() function of libopenaptx library. */
	if (ffb_init_int32_t(&pcm, (t->mtu_read / 6 + 1) * 8) == -1 ||
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

		const uint8_t *rtp_payload;
		const rtp_header_t *rtp_header = bt.data;
		if ((rtp_payload = rtp_a2dp_get_payload(rtp_header)) == NULL)
			continue;

		int missing_rtp_frames = 0;
		rtp_state_sync_stream(&rtp, rtp_header, &missing_rtp_frames, NULL);

		if (!ba_transport_pcm_is_active(t_pcm)) {
			rtp.synced = false;
			continue;
		}

		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		ffb_rewind(&pcm);
		while (rtp_payload_len >= 6) {

			size_t decoded = ffb_len_in(&pcm);
			if ((len = aptxhddec_decode(handle, rtp_payload, rtp_payload_len, pcm.tail, &decoded)) <= 0) {
				error("Apt-X decoding error: %s", strerror(errno));
				continue;
			}

			rtp_payload += len;
			rtp_payload_len -= len;
			ffb_seek(&pcm, decoded);

		}

		const size_t samples = ffb_len_out(&pcm);
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
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

static int a2dp_aptx_hd_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_aptx_hd_t *caps = capabilities;
	const a2dp_aptx_hd_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_aptx_hd_caps_intersect(caps, &sep->config.capabilities);

	unsigned int sampling_freq = 0;
	if (a2dp_aptx_hd_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		caps->aptx.sampling_freq = sampling_freq;
	else {
		error("apt-X HD: No supported sample rates: %#x", saved.aptx.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	unsigned int channel_mode = 0;
	if (a2dp_aptx_hd_caps_foreach_channel_mode(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode) != -1)
		caps->aptx.channel_mode = channel_mode;
	else {
		error("apt-X HD: No supported channel modes: %#x", saved.aptx.channel_mode);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_aptx_hd_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_aptx_hd_t *conf = configuration;
	a2dp_aptx_hd_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_aptx_hd_caps_intersect(&conf_v, &sep->config.capabilities);

	if (a2dp_bit_mapping_lookup(a2dp_aptx_rates, conf_v.aptx.sampling_freq) == -1) {
		debug("apt-X HD: Invalid sample rate: %#x", conf->aptx.sampling_freq);
		return A2DP_CHECK_ERR_RATE;
	}

	if (a2dp_bit_mapping_lookup(a2dp_aptx_channels, conf_v.aptx.channel_mode) == -1) {
		debug("apt-X HD: Invalid channel mode: %#x", conf->aptx.channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_aptx_hd_transport_init(struct ba_transport *t) {

	ssize_t channels_i;
	if ((channels_i = a2dp_bit_mapping_lookup(a2dp_aptx_channels,
					t->media.configuration.aptx_hd.aptx.channel_mode)) == -1)
		return -1;

	ssize_t rate_i;
	if ((rate_i = a2dp_bit_mapping_lookup(a2dp_aptx_rates,
					t->media.configuration.aptx_hd.aptx.sampling_freq)) == -1)
		return -1;

	t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S24_4LE;
	t->media.pcm.channels = a2dp_aptx_channels[channels_i].value;
	t->media.pcm.rate = a2dp_aptx_rates[rate_i].value;

	memcpy(t->media.pcm.channel_map, a2dp_aptx_channels[channels_i].ch.map,
			t->media.pcm.channels * sizeof(*t->media.pcm.channel_map));

	return 0;
}

static int a2dp_aptx_hd_source_init(struct a2dp_sep *sep) {
	if (config.a2dp.force_mono)
		warn("apt-X HD: Mono channel mode not supported");
	if (config.a2dp.force_44100)
		sep->config.capabilities.aptx_hd.aptx.sampling_freq = APTX_SAMPLING_FREQ_44100;
	return 0;
}

static int a2dp_aptx_hd_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_aptx_hd_enc_thread, "ba-a2dp-aptx-hd");
}

struct a2dp_sep a2dp_aptx_hd_source = {
	.name = "A2DP Source (apt-X HD)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
		.caps_size = sizeof(a2dp_aptx_hd_t),
		.capabilities.aptx_hd = {
			.aptx.info = A2DP_VENDOR_INFO_INIT(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
			/* NOTE: Used apt-X HD library does not support
			 *       single channel (mono) mode. */
			.aptx.channel_mode =
				APTX_CHANNEL_MODE_STEREO,
			.aptx.sampling_freq =
				APTX_SAMPLING_FREQ_16000 |
				APTX_SAMPLING_FREQ_32000 |
				APTX_SAMPLING_FREQ_44100 |
				APTX_SAMPLING_FREQ_48000,
		},
	},
	.init = a2dp_aptx_hd_source_init,
	.configuration_select = a2dp_aptx_hd_configuration_select,
	.configuration_check = a2dp_aptx_hd_configuration_check,
	.transport_init = a2dp_aptx_hd_transport_init,
	.transport_start = a2dp_aptx_hd_source_transport_start,
	.caps_helpers = &a2dp_aptx_hd_caps_helpers,
};

#if HAVE_APTX_HD_DECODE

static int a2dp_aptx_hd_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_aptx_hd_dec_thread, "ba-a2dp-aptx-hd");
}

struct a2dp_sep a2dp_aptx_hd_sink = {
	.name = "A2DP Sink (apt-X HD)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
		.caps_size = sizeof(a2dp_aptx_hd_t),
		.capabilities.aptx_hd = {
			.aptx.info = A2DP_VENDOR_INFO_INIT(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
			/* NOTE: Used apt-X HD library does not support
			 *       single channel (mono) mode. */
			.aptx.channel_mode =
				APTX_CHANNEL_MODE_STEREO,
			.aptx.sampling_freq =
				APTX_SAMPLING_FREQ_16000 |
				APTX_SAMPLING_FREQ_32000 |
				APTX_SAMPLING_FREQ_44100 |
				APTX_SAMPLING_FREQ_48000,
		},
	},
	.configuration_select = a2dp_aptx_hd_configuration_select,
	.configuration_check = a2dp_aptx_hd_configuration_check,
	.transport_init = a2dp_aptx_hd_transport_init,
	.transport_start = a2dp_aptx_hd_sink_transport_start,
	.caps_helpers = &a2dp_aptx_hd_caps_helpers,
};

#endif
