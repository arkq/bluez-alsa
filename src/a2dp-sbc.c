/*
 * BlueALSA - a2dp-sbc.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-sbc.h"

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

#include <glib.h>
#include <sbc/sbc.h>

#include "a2dp.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "ba-config.h"
#include "bluealsa-dbus.h"
#include "codec-sbc.h"
#include "io.h"
#include "rtp.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_bit_mapping a2dp_sbc_channels[] = {
	{ SBC_CHANNEL_MODE_MONO, .ch = { 1, a2dp_channel_map_mono } },
	{ SBC_CHANNEL_MODE_DUAL_CHANNEL, .ch = { 2, a2dp_channel_map_stereo } },
	{ SBC_CHANNEL_MODE_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ SBC_CHANNEL_MODE_JOINT_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ 0 },
};

static const struct a2dp_bit_mapping a2dp_sbc_rates[] = {
	{ SBC_SAMPLING_FREQ_16000, { 16000 } },
	{ SBC_SAMPLING_FREQ_32000, { 32000 } },
	{ SBC_SAMPLING_FREQ_44100, { 44100 } },
	{ SBC_SAMPLING_FREQ_48000, { 48000 } },
	{ 0 },
};

static void a2dp_sbc_caps_intersect(
		void *capabilities,
		const void *mask) {

	const a2dp_sbc_t *caps_mask = mask;
	a2dp_sbc_t *caps = capabilities;

	uint8_t min = MAX(caps->min_bitpool, caps_mask->min_bitpool);
	uint8_t max = MIN(caps->max_bitpool, caps_mask->max_bitpool);

	a2dp_caps_bitwise_intersect(caps, caps_mask, sizeof(*caps));
	caps->min_bitpool = min;
	caps->max_bitpool = max;

}

static int a2dp_sbc_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_sbc_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_sbc_channels, caps->channel_mode, func, userdata);
	return -1;
}

static int a2dp_sbc_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_sbc_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_sbc_rates, caps->sampling_freq, func, userdata);
	return -1;
}

static void a2dp_sbc_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	a2dp_sbc_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->channel_mode = a2dp_bit_mapping_lookup_value(a2dp_sbc_channels,
				caps->channel_mode, channels);
}

static void a2dp_sbc_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_sbc_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_sbc_rates,
				caps->sampling_freq, rate);
}

static struct a2dp_caps_helpers a2dp_sbc_caps_helpers = {
	.intersect = a2dp_sbc_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_sbc_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_sbc_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_sbc_caps_select_channel_mode,
	.select_sample_rate = a2dp_sbc_caps_select_sample_rate,
};

void *a2dp_sbc_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	sbc_t sbc;
	const a2dp_sbc_t *configuration = &t->media.configuration.sbc;
	if ((errno = -sbc_init_a2dp(&sbc, 0, configuration, sizeof(*configuration))) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);

	const size_t sbc_frame_pcm_samples = sbc_get_codesize(&sbc) / sizeof(int16_t);
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	/* initialize SBC encoder bit-pool */
	sbc.bitpool = sbc_a2dp_get_bitpool(configuration, config.sbc_quality);
	/* ensure libsbc uses little-endian PCM on all architectures */
	sbc.endian = SBC_LE;

#if DEBUG
	sbc_print_internals(&sbc);
#endif

	/* Writing MTU should be big enough to contain RTP header, SBC payload
	 * header and at least one SBC frame. In general, there is no constraint
	 * for the MTU value, but the speed might suffer significantly. */
	const size_t rtp_headers_len = RTP_HEADER_LEN + sizeof(rtp_media_header_t);
	const size_t mtu_write_payload_len = t->mtu_write - rtp_headers_len;
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);

	size_t ffb_pcm_len = sbc_frame_pcm_samples;
	if (mtu_write_payload_len / sbc_frame_len > 1)
		/* account for possible SBC frames packing */
		ffb_pcm_len *= mtu_write_payload_len / sbc_frame_len;

	if (mtu_write_payload_len < sbc_frame_len)
		warn("Writing MTU too small for one single SBC frame: %zu < %zu",
				t->mtu_write, RTP_HEADER_LEN + sizeof(rtp_media_header_t) + sbc_frame_len);

	if (ffb_init_int16_t(&pcm, ffb_pcm_len) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	const unsigned int sbc_delay_pcm_frames = 73;
	/* Get the total delay introduced by the codec. */
	t_pcm->codec_delay_dms = sbc_delay_pcm_frames * 10000 / rate;
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
				sbc_reinit_a2dp(&sbc, 0, configuration, sizeof(*configuration));
				sbc.bitpool = sbc_a2dp_get_bitpool(configuration, config.sbc_quality);
				sbc.endian = SBC_LE;
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
		size_t output_len = ffb_len_in(&bt);
		size_t pcm_frames = 0;
		size_t sbc_frames = 0;

		/* Generate as many SBC frames as possible, but less than a 4-bit media
		 * header frame counter can contain. The size of the output buffer is
		 * based on the socket MTU, so such transfer should be most efficient. */
		while (input_samples >= sbc_frame_pcm_samples &&
				output_len >= sbc_frame_len &&
				/* do not overflow RTP frame counter */
				sbc_frames < ((1 << 4) - 1)) {

			ssize_t len;
			ssize_t encoded;

			if ((len = sbc_encode(&sbc, input, input_samples * sizeof(int16_t),
							bt.tail, output_len, &encoded)) < 0) {
				error("SBC encoding error: %s", sbc_strerror(len));
				break;
			}

			len = len / sizeof(int16_t);
			input += len;
			input_samples -= len;
			ffb_seek(&bt, encoded);
			output_len -= encoded;
			pcm_frames += len / channels;
			sbc_frames++;

		}

		if (sbc_frames > 0) {

			rtp_state_new_frame(&rtp, rtp_header);
			rtp_media_header->frame_count = sbc_frames;

			ssize_t len = ffb_blen_out(&bt);
			if ((len = io_bt_write(t_pcm, bt.data, len)) <= 0) {
				if (len == -1)
					error("BT write error: %s", strerror(errno));
				goto fail;
			}

			if (!io.initiated) {
				/* Get the codec processing delay, which is a time spent in the
				 * processing loop between reading PCM data and writing the first
				 * encoded SBC frame. Subsequently encoded frames do not contribute
				 * to the delay, because (assuming no underruns) since the first
				 * frame is written, the BT sink can start decoding and playing
				 * audio in a continuous fashion. */
				t_pcm->processing_delay_dms = asrsync_get_dms_since_last_sync(&io.asrs);
				ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);
				io.initiated = true;
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
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

__attribute__ ((weak))
void *a2dp_sbc_dec_thread(struct ba_transport_pcm *t_pcm) {

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	sbc_t sbc;
	if ((errno = -sbc_init_a2dp(&sbc, 0, &t->media.configuration.sbc,
					sizeof(t->media.configuration.sbc))) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	/* ensure libsbc uses little-endian PCM on all architectures */
	sbc.endian = SBC_LE;

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	if (ffb_init_int16_t(&pcm, sbc_get_codesize(&sbc)) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to PCM sample rate */
	rtp_state_init(&rtp, rate, rate);

#if DEBUG
	uint16_t sbc_bitpool = 0;
#endif

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

		/* decode retrieved SBC frames */
		size_t frames = rtp_media_header->frame_count;
		while (frames--) {

			size_t decoded;
			if ((len = sbc_decode(&sbc, rtp_payload, rtp_payload_len,
							pcm.data, ffb_blen_in(&pcm), &decoded)) < 0) {
				error("SBC decoding error: %s", sbc_strerror(len));
				break;
			}

#if DEBUG
			if (sbc_bitpool != sbc.bitpool) {
				sbc_bitpool = sbc.bitpool;
				sbc_print_internals(&sbc);
			}
#endif

			rtp_payload += len;
			rtp_payload_len -= len;

			const size_t samples = decoded / sizeof(int16_t);
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("PCM write error: %s", strerror(errno));

			/* update local state with decoded PCM frames */
			rtp_state_update(&rtp, samples / channels);

		}

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

static int a2dp_sbc_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_sbc_t *caps = capabilities;
	const a2dp_sbc_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_sbc_caps_intersect(caps, &sep->config.capabilities);

	unsigned int sampling_freq = 0;
	if (a2dp_sbc_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) == -1) {
		error("SBC: No supported sample rates: %#x", saved.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	unsigned int channel_mode = 0;
	if (a2dp_sbc_caps_foreach_channel_mode(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode) == -1) {
		error("SBC: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	if (config.sbc_quality == SBC_QUALITY_XQ ||
			config.sbc_quality == SBC_QUALITY_XQPLUS) {
		if (caps->sampling_freq & SBC_SAMPLING_FREQ_44100)
			sampling_freq = SBC_SAMPLING_FREQ_44100;
		else
			warn("SBC XQ: 44.1 kHz sample rate not supported: %#x", saved.sampling_freq);
		if (caps->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
			channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
		else
			warn("SBC XQ: Dual channel mode not supported: %#x", saved.channel_mode);
	}

	caps->sampling_freq = sampling_freq;
	caps->channel_mode = channel_mode;

	if (caps->block_length & SBC_BLOCK_LENGTH_16)
		caps->block_length = SBC_BLOCK_LENGTH_16;
	else if (caps->block_length & SBC_BLOCK_LENGTH_12)
		caps->block_length = SBC_BLOCK_LENGTH_12;
	else if (caps->block_length & SBC_BLOCK_LENGTH_8)
		caps->block_length = SBC_BLOCK_LENGTH_8;
	else if (caps->block_length & SBC_BLOCK_LENGTH_4)
		caps->block_length = SBC_BLOCK_LENGTH_4;
	else {
		error("SBC: No supported block lengths: %#x", saved.block_length);
		return errno = ENOTSUP, -1;
	}

	if (caps->subbands & SBC_SUBBANDS_8)
		caps->subbands = SBC_SUBBANDS_8;
	else if (caps->subbands & SBC_SUBBANDS_4)
		caps->subbands = SBC_SUBBANDS_4;
	else {
		error("SBC: No supported sub-bands: %#x", saved.subbands);
		return errno = ENOTSUP, -1;
	}

	if (caps->allocation_method & SBC_ALLOCATION_LOUDNESS)
		caps->allocation_method = SBC_ALLOCATION_LOUDNESS;
	else if (caps->allocation_method & SBC_ALLOCATION_SNR)
		caps->allocation_method = SBC_ALLOCATION_SNR;
	else {
		error("SBC: No supported allocation methods: %#x", saved.allocation_method);
		return errno = ENOTSUP, -1;
	}

	if (caps->min_bitpool > caps->max_bitpool) {
		error("SBC: No supported bit-pool range: [%u, %u]",
				saved.min_bitpool, saved.max_bitpool);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_sbc_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_sbc_t *conf = configuration;
	a2dp_sbc_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_sbc_caps_intersect(&conf_v, &sep->config.capabilities);

	if (a2dp_bit_mapping_lookup(a2dp_sbc_rates, conf_v.sampling_freq) == -1) {
		debug("SBC: Invalid sample rate: %#x", conf->sampling_freq);
		return A2DP_CHECK_ERR_RATE;
	}

	if (a2dp_bit_mapping_lookup(a2dp_sbc_channels, conf_v.channel_mode) == -1) {
		debug("SBC: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	switch (conf_v.block_length) {
	case SBC_BLOCK_LENGTH_4:
	case SBC_BLOCK_LENGTH_8:
	case SBC_BLOCK_LENGTH_12:
	case SBC_BLOCK_LENGTH_16:
		break;
	default:
		debug("SBC: Invalid block length: %#x", conf->block_length);
		return A2DP_CHECK_ERR_BLOCK_LENGTH;
	}

	switch (conf_v.subbands) {
	case SBC_SUBBANDS_4:
	case SBC_SUBBANDS_8:
		break;
	default:
		debug("SBC: Invalid sub-bands: %#x", conf->subbands);
		return A2DP_CHECK_ERR_SUB_BANDS;
	}

	switch (conf_v.allocation_method) {
	case SBC_ALLOCATION_SNR:
	case SBC_ALLOCATION_LOUDNESS:
		break;
	default:
		debug("SBC: Invalid allocation method: %#x", conf->allocation_method);
		return A2DP_CHECK_ERR_ALLOCATION_METHOD;
	}

	if (conf_v.min_bitpool > conf_v.max_bitpool) {
		error("SBC: Invalid bit-pool range: [%u, %u]",
				conf->min_bitpool, conf->max_bitpool);
		return A2DP_CHECK_ERR_BIT_POOL_RANGE;
	}

	debug("SBC: Selected bit-pool range: [%u, %u]",
			conf->min_bitpool, conf->max_bitpool);

	return A2DP_CHECK_OK;
}

static int a2dp_sbc_transport_init(struct ba_transport *t) {

	ssize_t channels_i;
	if ((channels_i = a2dp_bit_mapping_lookup(a2dp_sbc_channels,
					t->media.configuration.sbc.channel_mode)) == -1)
		return -1;

	ssize_t rate_i;
	if ((rate_i = a2dp_bit_mapping_lookup(a2dp_sbc_rates,
					t->media.configuration.sbc.sampling_freq)) == -1)
		return -1;

	t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->media.pcm.channels = a2dp_sbc_channels[channels_i].value;
	t->media.pcm.rate = a2dp_sbc_rates[rate_i].value;

	memcpy(t->media.pcm.channel_map, a2dp_sbc_channels[channels_i].ch.map,
			t->media.pcm.channels * sizeof(*t->media.pcm.channel_map));

	return 0;
}

static int a2dp_sbc_source_init(struct a2dp_sep *sep) {

	if (config.sbc_quality == SBC_QUALITY_XQ ||
			config.sbc_quality == SBC_QUALITY_XQPLUS) {
		info("SBC: Activating SBC Dual Channel HD (SBC %s)",
				config.sbc_quality == SBC_QUALITY_XQ ? "XQ" : "XQ+");
		sep->config.capabilities.sbc.sampling_freq = SBC_SAMPLING_FREQ_44100;
		sep->config.capabilities.sbc.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
	}

	if (config.a2dp.force_mono)
		/* With this we are violating A2DP SBC requirements. According to spec,
		 * SBC source shall support mono channel and at least one of the stereo
		 * modes. However, since for sink all channel modes are mandatory, even
		 * though we are supporting only mono mode, there will be a match when
		 * selecting configuration. */
		sep->config.capabilities.sbc.channel_mode = SBC_CHANNEL_MODE_MONO;
	if (config.a2dp.force_44100)
		sep->config.capabilities.sbc.sampling_freq = SBC_SAMPLING_FREQ_44100;

	return 0;
}

static int a2dp_sbc_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_sbc_enc_thread, "ba-a2dp-sbc");
}

struct a2dp_sep a2dp_sbc_source = {
	.name = "A2DP Source (SBC)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_SBC,
		.caps_size = sizeof(a2dp_sbc_t),
		.capabilities.sbc = {
			.sampling_freq =
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
		},
	},
	.init = a2dp_sbc_source_init,
	.configuration_select = a2dp_sbc_configuration_select,
	.configuration_check = a2dp_sbc_configuration_check,
	.transport_init = a2dp_sbc_transport_init,
	.transport_start = a2dp_sbc_source_transport_start,
	.caps_helpers = &a2dp_sbc_caps_helpers,
	.enabled = true,
};

static int a2dp_sbc_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_sbc_dec_thread, "ba-a2dp-sbc");
}

struct a2dp_sep a2dp_sbc_sink = {
	.name = "A2DP Sink (SBC)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_SBC,
		.caps_size = sizeof(a2dp_sbc_t),
		.capabilities.sbc = {
			.sampling_freq =
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
		},
	},
	.configuration_select = a2dp_sbc_configuration_select,
	.configuration_check = a2dp_sbc_configuration_check,
	.transport_init = a2dp_sbc_transport_init,
	.transport_start = a2dp_sbc_sink_transport_start,
	.caps_helpers = &a2dp_sbc_caps_helpers,
	.enabled = true,
};
