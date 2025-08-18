/*
 * BlueALSA - a2dp-faststream.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-faststream.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sbc/sbc.h>

#include "a2dp.h"
#include "ba-config.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "codec-sbc.h"
#include "io.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_bit_mapping a2dp_fs_rates_music[] = {
	{ FASTSTREAM_SAMPLING_FREQ_MUSIC_44100, { 44100 } },
	{ FASTSTREAM_SAMPLING_FREQ_MUSIC_48000, { 48000 } },
	{ 0 }
};

static const struct a2dp_bit_mapping a2dp_fs_rates_voice[] = {
	{ FASTSTREAM_SAMPLING_FREQ_VOICE_16000, { 16000 } },
	{ 0 }
};

static void a2dp_fs_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_faststream_t));
}

static bool a2dp_fs_caps_has_stream(
		const void *capabilities,
		enum a2dp_stream stream) {
	const a2dp_faststream_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return caps->direction & FASTSTREAM_DIRECTION_MUSIC;
	return caps->direction & FASTSTREAM_DIRECTION_VOICE;
}

static int a2dp_fs_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	(void)capabilities;
	const struct a2dp_bit_mapping channels_mono = {
		.ch = { 1, a2dp_channel_map_mono } };
	const struct a2dp_bit_mapping channels_stereo = {
		.ch = { 2, a2dp_channel_map_stereo } };
	if (stream == A2DP_MAIN)
		return func(channels_stereo, userdata);
	return func(channels_mono, userdata);
}

static int a2dp_fs_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_faststream_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_fs_rates_music,
				caps->sampling_freq_music, func, userdata);
	return a2dp_bit_mapping_foreach(a2dp_fs_rates_voice,
			caps->sampling_freq_voice, func, userdata);
}

static void a2dp_fs_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	(void)capabilities;
	(void)stream;
	(void)channels;
}

static void a2dp_fs_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_faststream_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->sampling_freq_music = a2dp_bit_mapping_lookup_value(a2dp_fs_rates_music,
				caps->sampling_freq_music, rate);
	else
		caps->sampling_freq_voice = a2dp_bit_mapping_lookup_value(a2dp_fs_rates_voice,
				caps->sampling_freq_voice, rate);
}

static struct a2dp_caps_helpers a2dp_fs_caps_helpers = {
	.intersect = a2dp_fs_caps_intersect,
	.has_stream = a2dp_fs_caps_has_stream,
	.foreach_channel_mode = a2dp_fs_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_fs_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_fs_caps_select_channel_mode,
	.select_sample_rate = a2dp_fs_caps_select_sample_rate,
};

void *a2dp_fs_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	/* determine encoder operation mode: music or voice */
	const bool is_voice = t->profile & BA_TRANSPORT_PROFILE_A2DP_SINK;

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	sbc_t sbc;
	const a2dp_faststream_t *configuration = &t->media.configuration.faststream;
	if ((errno = -sbc_init_a2dp_faststream(&sbc, 0, configuration,
					sizeof(*configuration), is_voice)) != 0) {
		error("Couldn't initialize FastStream SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);

	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);
	const size_t sbc_frame_pcm_samples = sbc_get_codesize(&sbc) / sizeof(int16_t);
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	if (ffb_init_int16_t(&pcm, sbc_frame_pcm_samples * 3) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	const unsigned int sbc_delay_pcm_frames = 73;
	/* Get the total delay introduced by the codec. */
	t_pcm->codec_delay_dms = sbc_delay_pcm_frames * 10000 / rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				sbc_reinit_a2dp_faststream(&sbc, 0, configuration,
						sizeof(*configuration), is_voice);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		const int16_t *input = pcm.data;
		size_t input_len = ffb_len_out(&pcm);
		size_t output_len = ffb_len_in(&bt);
		size_t pcm_frames = 0;
		size_t sbc_frames = 0;

		while (input_len >= sbc_frame_pcm_samples &&
				output_len >= sbc_frame_len &&
				sbc_frames < 3) {

			ssize_t len;
			ssize_t encoded;

			if ((len = sbc_encode(&sbc, input, input_len * sizeof(int16_t),
							bt.tail, output_len, &encoded)) < 0) {
				error("FastStream SBC encoding error: %s", sbc_strerror(len));
				break;
			}

			len = len / sizeof(int16_t);
			input += len;
			input_len -= len;
			ffb_seek(&bt, encoded);
			output_len -= encoded;
			pcm_frames += len / channels;
			sbc_frames += 1;

		}

		if (sbc_frames > 0) {

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

			/* make room for new FastStream frames */
			ffb_rewind(&bt);

			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, pcm_frames);

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
void *a2dp_fs_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	/* determine decoder operation mode: music or voice */
	const bool is_voice = t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE;

	sbc_t sbc;
	if ((errno = -sbc_init_a2dp_faststream(&sbc, 0, &t->media.configuration.faststream,
					sizeof(t->media.configuration.faststream), is_voice)) != 0) {
		error("Couldn't initialize FastStream SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);
	const size_t sbc_frame_pcm_samples = sbc_get_codesize(&sbc) / sizeof(int16_t);

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int16_t(&pcm, sbc_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len;
		ffb_rewind(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(t_pcm))
			continue;

		uint8_t *input = bt.data;
		size_t input_len = len;

		/* decode retrieved SBC frames */
		while (input_len >= sbc_frame_len) {

			size_t decoded;
			if ((len = sbc_decode(&sbc, input, input_len,
							pcm.data, ffb_blen_in(&pcm), &decoded)) < 0) {
				error("FastStream SBC decoding error: %s", sbc_strerror(len));
				break;
			}

			input += len;
			input_len -= len;

			const size_t samples = decoded / sizeof(int16_t);
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("PCM write error: %s", strerror(errno));

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

static int a2dp_fs_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_faststream_t *caps = capabilities;
	const a2dp_faststream_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_fs_caps_intersect(caps, &sep->config.capabilities);

	if ((caps->direction & (FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE)) == 0) {
		error("FastStream: No supported directions: %#x", saved.direction);
		return errno = ENOTSUP, -1;
	}

	unsigned int sampling_freq_v = 0;
	if (caps->direction & FASTSTREAM_DIRECTION_VOICE &&
			a2dp_fs_caps_foreach_sample_rate(caps, A2DP_BACKCHANNEL,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq_v) != -1)
		caps->sampling_freq_voice = sampling_freq_v;
	else {
		error("FastStream: No supported voice sample rates: %#x", saved.sampling_freq_voice);
		return errno = ENOTSUP, -1;
	}

	unsigned int sampling_freq_m = 0;
	if (caps->direction & FASTSTREAM_DIRECTION_MUSIC &&
			a2dp_fs_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq_m) != -1)
		caps->sampling_freq_music = sampling_freq_m;
	else {
		error("FastStream: No supported music sample rates: %#x", saved.sampling_freq_music);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_fs_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_faststream_t *conf = configuration;
	a2dp_faststream_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_fs_caps_intersect(&conf_v, &sep->config.capabilities);

	if ((conf_v.direction & (FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE)) == 0) {
		debug("FastStream: Invalid direction: %#x", conf->direction);
		return A2DP_CHECK_ERR_DIRECTIONS;
	}

	if (conf_v.direction & FASTSTREAM_DIRECTION_VOICE &&
			a2dp_bit_mapping_lookup(a2dp_fs_rates_voice, conf_v.sampling_freq_voice) == -1) {
		debug("FastStream: Invalid voice sample rate: %#x", conf->sampling_freq_voice);
		return A2DP_CHECK_ERR_RATE_VOICE;
	}

	if (conf_v.direction & FASTSTREAM_DIRECTION_MUSIC &&
			a2dp_bit_mapping_lookup(a2dp_fs_rates_music, conf_v.sampling_freq_music) == -1) {
		debug("FastStream: Invalid music sample rate: %#x", conf->sampling_freq_music);
		return A2DP_CHECK_ERR_RATE_MUSIC;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_fs_transport_init(struct ba_transport *t) {

	if (t->media.configuration.faststream.direction & FASTSTREAM_DIRECTION_MUSIC) {

		ssize_t rate_i;
		if ((rate_i = a2dp_bit_mapping_lookup(a2dp_fs_rates_music,
						t->media.configuration.faststream.sampling_freq_music)) == -1)
			return -1;

		t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
		t->media.pcm.channels = 2;
		t->media.pcm.rate = a2dp_fs_rates_music[rate_i].value;

		memcpy(t->media.pcm.channel_map, a2dp_channel_map_stereo,
				2 * sizeof(*a2dp_channel_map_stereo));

	}

	if (t->media.configuration.faststream.direction & FASTSTREAM_DIRECTION_VOICE) {

		ssize_t rate_i;
		if ((rate_i = a2dp_bit_mapping_lookup(a2dp_fs_rates_voice,
						t->media.configuration.faststream.sampling_freq_voice)) == -1)
			return -1;

		t->media.pcm_bc.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
		t->media.pcm_bc.channels = 1;
		t->media.pcm_bc.rate = a2dp_fs_rates_voice[rate_i].value;

		memcpy(t->media.pcm_bc.channel_map, a2dp_channel_map_mono,
				1 * sizeof(*a2dp_channel_map_stereo));

	}

	return 0;
}

static int a2dp_fs_source_init(struct a2dp_sep *sep) {
	if (config.a2dp.force_mono)
		warn("FastStream: Mono channel mode not supported");
	if (config.a2dp.force_44100)
		sep->config.capabilities.faststream.sampling_freq_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_44100;
	return 0;
}

static int a2dp_fs_source_transport_start(struct ba_transport *t) {

	struct ba_transport_pcm *pcm = &t->media.pcm;
	struct ba_transport_pcm *pcm_bc = &t->media.pcm_bc;
	int rv = 0;

	if (t->media.configuration.faststream.direction & FASTSTREAM_DIRECTION_MUSIC)
		rv |= ba_transport_pcm_start(pcm, a2dp_fs_enc_thread, "ba-a2dp-fs-m");
	if (t->media.configuration.faststream.direction & FASTSTREAM_DIRECTION_VOICE)
		rv |= ba_transport_pcm_start(pcm_bc, a2dp_fs_dec_thread, "ba-a2dp-fs-v");

	return rv;
}

struct a2dp_sep a2dp_faststream_source = {
	.name = "A2DP Source (FastStream)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
		.caps_size = sizeof(a2dp_faststream_t),
		.capabilities.faststream = {
			.info = A2DP_VENDOR_INFO_INIT(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
			.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE,
			.sampling_freq_music =
				FASTSTREAM_SAMPLING_FREQ_MUSIC_44100 |
				FASTSTREAM_SAMPLING_FREQ_MUSIC_48000,
			.sampling_freq_voice =
				FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
		},
	},
	.init = a2dp_fs_source_init,
	.configuration_select = a2dp_fs_configuration_select,
	.configuration_check = a2dp_fs_configuration_check,
	.transport_init = a2dp_fs_transport_init,
	.transport_start = a2dp_fs_source_transport_start,
	.caps_helpers = &a2dp_fs_caps_helpers,
};

static int a2dp_fs_sink_transport_start(struct ba_transport *t) {

	struct ba_transport_pcm *pcm = &t->media.pcm;
	struct ba_transport_pcm *pcm_bc = &t->media.pcm_bc;
	int rv = 0;

	if (t->media.configuration.faststream.direction & FASTSTREAM_DIRECTION_MUSIC)
		rv |= ba_transport_pcm_start(pcm, a2dp_fs_dec_thread, "ba-a2dp-fs-m");
	if (t->media.configuration.faststream.direction & FASTSTREAM_DIRECTION_VOICE)
		rv |= ba_transport_pcm_start(pcm_bc, a2dp_fs_enc_thread, "ba-a2dp-fs-v");

	return rv;
}

struct a2dp_sep a2dp_faststream_sink = {
	.name = "A2DP Sink (FastStream)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
		.caps_size = sizeof(a2dp_faststream_t),
		.capabilities.faststream = {
			.info = A2DP_VENDOR_INFO_INIT(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
			.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE,
			.sampling_freq_music =
				FASTSTREAM_SAMPLING_FREQ_MUSIC_44100 |
				FASTSTREAM_SAMPLING_FREQ_MUSIC_48000,
			.sampling_freq_voice =
				FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
		},
	},
	.configuration_select = a2dp_fs_configuration_select,
	.configuration_check = a2dp_fs_configuration_check,
	.transport_init = a2dp_fs_transport_init,
	.transport_start = a2dp_fs_sink_transport_start,
	.caps_helpers = &a2dp_fs_caps_helpers,
};
