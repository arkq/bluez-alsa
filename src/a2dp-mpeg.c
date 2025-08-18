/*
 * BlueALSA - a2dp-mpeg.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-mpeg.h"

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

#if ENABLE_MP3LAME
# include <lame/lame.h>
#endif
#if ENABLE_MPG123
# include <mpg123.h>
#endif

#include "a2dp.h"
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

static const struct a2dp_bit_mapping a2dp_mpeg_channels[] = {
	{ MPEG_CHANNEL_MODE_MONO, .ch = { 1, a2dp_channel_map_mono } },
	{ MPEG_CHANNEL_MODE_DUAL_CHANNEL, .ch = { 2, a2dp_channel_map_stereo } },
	{ MPEG_CHANNEL_MODE_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ MPEG_CHANNEL_MODE_JOINT_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ 0 }
};

static const struct a2dp_bit_mapping a2dp_mpeg_rates[] = {
	{ MPEG_SAMPLING_FREQ_16000, { 16000 } },
	{ MPEG_SAMPLING_FREQ_22050, { 22050 } },
	{ MPEG_SAMPLING_FREQ_24000, { 24000 } },
	{ MPEG_SAMPLING_FREQ_32000, { 32000 } },
	{ MPEG_SAMPLING_FREQ_44100, { 44100 } },
	{ MPEG_SAMPLING_FREQ_48000, { 48000 } },
	{ 0 }
};

static void a2dp_mpeg_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_mpeg_t));
}

static int a2dp_mpeg_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_mpeg_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_mpeg_channels, caps->channel_mode, func, userdata);
	return -1;
}

static int a2dp_mpeg_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_mpeg_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_mpeg_rates, caps->sampling_freq, func, userdata);
	return -1;
}

static void a2dp_mpeg_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	a2dp_mpeg_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->channel_mode = a2dp_bit_mapping_lookup_value(a2dp_mpeg_channels,
				caps->channel_mode, channels);
}

static void a2dp_mpeg_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_mpeg_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_mpeg_rates,
				caps->sampling_freq, rate);
}

static struct a2dp_caps_helpers a2dp_mpeg_caps_helpers = {
	.intersect = a2dp_mpeg_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_mpeg_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_mpeg_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_mpeg_caps_select_channel_mode,
	.select_sample_rate = a2dp_mpeg_caps_select_sample_rate,
};

#if ENABLE_MP3LAME
void *a2dp_mp3_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	lame_t handle;
	if ((handle = lame_init()) == NULL) {
		error("Couldn't initialize LAME encoder: %s", strerror(errno));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(lame_close), handle);

	const a2dp_mpeg_t *configuration = &t->media.configuration.mpeg;
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	MPEG_mode mode = NOT_SET;

	lame_set_num_channels(handle, channels);
	lame_set_in_samplerate(handle, rate);

	switch (configuration->channel_mode) {
	case MPEG_CHANNEL_MODE_MONO:
		mode = MONO;
		break;
	case MPEG_CHANNEL_MODE_DUAL_CHANNEL:
		mode = DUAL_CHANNEL;
		break;
	case MPEG_CHANNEL_MODE_STEREO:
		mode = STEREO;
		break;
	case MPEG_CHANNEL_MODE_JOINT_STEREO:
		mode = JOINT_STEREO;
		break;
	}

	if (lame_set_mode(handle, mode) != 0) {
		error("LAME: Couldn't set mode: %d", mode);
		goto fail_setup;
	}
	if (lame_set_bWriteVbrTag(handle, 0) != 0) {
		error("LAME: Couldn't disable VBR header");
		goto fail_setup;
	}
	if (lame_set_error_protection(handle, configuration->crc) != 0) {
		error("LAME: Couldn't set CRC mode: %d", configuration->crc);
		goto fail_setup;
	}
	if (configuration->vbr) {
		if (lame_set_VBR(handle, vbr_default) != 0) {
			error("LAME: Couldn't set VBR mode: %d", vbr_default);
			goto fail_setup;
		}
		if (lame_set_VBR_q(handle, config.lame_vbr_quality) != 0) {
			error("LAME: Couldn't set VBR quality: %d", config.lame_vbr_quality);
			goto fail_setup;
		}
	}
	else {
		if (lame_set_VBR(handle, vbr_off) != 0) {
			error("LAME: Couldn't set CBR mode");
			goto fail_setup;
		}
		int mpeg_bitrate = A2DP_MPEG_GET_BITRATE(*configuration);
		int bitrate = a2dp_mpeg1_mp3_get_max_bitrate(mpeg_bitrate);
		if (lame_set_brate(handle, bitrate) != 0) {
			error("LAME: Couldn't set CBR bitrate: %d", bitrate);
			goto fail_setup;
		}
		if (mpeg_bitrate & MPEG_BITRATE_FREE &&
				lame_set_free_format(handle, 1) != 0) {
			error("LAME: Couldn't enable free format");
			goto fail_setup;
		}
	}
	if (lame_set_quality(handle, config.lame_quality) != 0) {
		error("LAME: Couldn't set quality: %d", config.lame_quality);
		goto fail_setup;
	}

	if (lame_init_params(handle) != 0) {
		error("LAME: Couldn't setup encoder");
		goto fail_setup;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const size_t mpeg_frame_pcm_samples = lame_get_framesize(handle);
	const size_t rtp_headers_len = RTP_HEADER_LEN + sizeof(rtp_mpeg_audio_header_t);
	/* It is hard to tell the size of the buffer required, but empirical test
	 * shows that 2KB should be sufficient for encoding. However, encoder flush
	 * function requires a little bit more space. */
	const size_t mpeg_frame_len = 4 * 1024;

	if (ffb_init_int16_t(&pcm, mpeg_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, rtp_headers_len + mpeg_frame_len) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	/* Get the total delay introduced by the codec. */
	const int mpeg_delay_pcm_frames = lame_get_encoder_delay(handle);
	t_pcm->codec_delay_dms = mpeg_delay_pcm_frames * 10000 / rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

	rtp_header_t *rtp_header;
	rtp_mpeg_audio_header_t *rtp_mpeg_audio_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_mpeg_audio_header, sizeof(*rtp_mpeg_audio_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to 90kHz */
	rtp_state_init(&rtp, rate, 90000);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				lame_encode_flush(handle, rtp_payload, mpeg_frame_len);
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

		size_t pcm_frames = ffb_len_out(&pcm) / channels;
		ssize_t len;

		if ((len = channels == 1 ?
					lame_encode_buffer(handle, pcm.data, NULL, pcm_frames, bt.tail, ffb_len_in(&bt)) :
					lame_encode_buffer_interleaved(handle, pcm.data, pcm_frames, bt.tail, ffb_len_in(&bt))) < 0) {
			error("LAME encoding error: %s", lame_encode_strerror(len));
			continue;
		}

		if (len > 0) {

			size_t payload_len_max = t->mtu_write - RTP_HEADER_LEN - sizeof(*rtp_mpeg_audio_header);
			size_t payload_len_total = len;
			size_t payload_len = len;

			for (;;) {

				size_t chunk_len;
				chunk_len = payload_len > payload_len_max ? payload_len_max : payload_len;
				rtp_header->markbit = payload_len <= payload_len_max;
				rtp_state_new_frame(&rtp, rtp_header);
				rtp_mpeg_audio_header->offset = payload_len_total - payload_len;

				ffb_rewind(&bt);
				ffb_seek(&bt, RTP_HEADER_LEN + sizeof(*rtp_mpeg_audio_header) + chunk_len);

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

				/* account written payload only */
				len -= RTP_HEADER_LEN + sizeof(*rtp_mpeg_audio_header);

				/* break if the last part of the payload has been written */
				if ((payload_len -= len) == 0)
					break;

				/* move rest of data to the beginning of the payload */
				debug("Payload fragmentation: extra %zd bytes", payload_len);
				memmove(rtp_payload, rtp_payload + len, payload_len);

			}

		}

		/* Keep data transfer at a constant bit rate. */
		asrsync_sync(&io.asrs, pcm_frames);
		/* move forward RTP timestamp clock */
		rtp_state_update(&rtp, pcm_frames);

		/* If the input buffer was not consumed (due to frame alignment), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, pcm_frames * channels);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_setup:
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_MPG123 || ENABLE_MP3LAME
__attribute__ ((weak))
void *a2dp_mpeg_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

#if ENABLE_MPG123

	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, (void (*)(void))mpg123_init);

	int err;
	mpg123_handle *handle;
	if ((handle = mpg123_new(NULL, &err)) == NULL) {
		error("Couldn't initialize MPG123 decoder: %s", mpg123_plain_strerror(err));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(mpg123_delete), handle);

	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	mpg123_param(handle, MPG123_RESYNC_LIMIT, -1, 0);
	mpg123_param(handle, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
#if MPG123_API_VERSION >= 45
	mpg123_param(handle, MPG123_ADD_FLAGS, MPG123_NO_READAHEAD, 0);
#endif

	mpg123_format_none(handle);
	if (mpg123_format(handle, rate, channels, MPG123_ENC_SIGNED_16) != MPG123_OK) {
		error("Couldn't set MPG123 format: %s", mpg123_strerror(handle));
		goto fail_open;
	}

	if (mpg123_open_feed(handle) != MPG123_OK) {
		error("Couldn't open MPG123 feed: %s", mpg123_strerror(handle));
		goto fail_open;
	}

	#define MPEG_PCM_DECODE_SAMPLES 4096

#else

	hip_t handle;
	if ((handle = hip_decode_init()) == NULL) {
		error("Couldn't initialize LAME decoder: %s", strerror(errno));
		goto fail_init;
	}

	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	pthread_cleanup_push(PTHREAD_CLEANUP(hip_decode_exit), handle);

	/* NOTE: Size of the output buffer is "hard-coded" in hip_decode(). What is
	 *       even worse, the boundary check is so fucked-up that the hard-coded
	 *       limit can very easily overflow. In order to mitigate crash, we are
	 *       going to provide very big buffer - let's hope it will be enough. */
	#define MPEG_PCM_DECODE_SAMPLES 4096 * 100

#endif

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int16_t(&pcm, MPEG_PCM_DECODE_SAMPLES) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to 90kHz */
	rtp_state_init(&rtp, rate, 90000);

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
		const rtp_mpeg_audio_header_t *rtp_mpeg_header;
		if ((rtp_mpeg_header = rtp_a2dp_get_payload(rtp_header)) == NULL)
			continue;

		int missing_rtp_frames = 0;
		rtp_state_sync_stream(&rtp, rtp_header, &missing_rtp_frames, NULL);

		if (!ba_transport_pcm_is_active(t_pcm)) {
			rtp.synced = false;
			continue;
		}

		uint8_t *rtp_mpeg = (uint8_t *)(rtp_mpeg_header + 1);
		size_t rtp_mpeg_len = len - (rtp_mpeg - (uint8_t *)bt.data);

#if ENABLE_MPG123

		long rate_;
		int channels_;
		int encoding;

decode:
		switch (mpg123_decode(handle, rtp_mpeg, rtp_mpeg_len,
					(uint8_t *)pcm.data, ffb_blen_in(&pcm), (size_t *)&len)) {
		case MPG123_DONE:
		case MPG123_NEED_MORE:
		case MPG123_OK:
			break;
		case MPG123_NEW_FORMAT:
			mpg123_getformat(handle, &rate_, &channels_, &encoding);
			debug("MPG123 new format detected: r:%ld, ch:%d, enc:%#x", rate_, channels_, encoding);
			break;
		default:
			error("MPG123 decoding error: %s", mpg123_strerror(handle));
			continue;
		}

		const size_t samples = len / sizeof(int16_t);
		io_pcm_scale(t_pcm, pcm.data, samples);
		if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
			error("PCM write error: %s", strerror(errno));

		/* update local state with decoded PCM frames */
		rtp_state_update(&rtp, samples / channels);

		if (len > 0) {
			rtp_mpeg_len = 0;
			goto decode;
		}

#else

		int16_t pcm_l[MPEG_PCM_DECODE_SAMPLES];
		int16_t pcm_r[MPEG_PCM_DECODE_SAMPLES];
		ssize_t samples;

		if ((samples = hip_decode(handle, rtp_mpeg, rtp_mpeg_len, pcm_l, pcm_r)) < 0) {
			error("LAME decoding error: %zd", samples);
			continue;
		}

		if (channels == 1) {
			io_pcm_scale(t_pcm, pcm_l, samples);
			if (io_pcm_write(t_pcm, pcm_l, samples) == -1)
				error("PCM write error: %s", strerror(errno));
		}
		else {

			for (ssize_t i = 0; i < samples; i++) {
				((int16_t *)pcm.data)[i * 2 + 0] = pcm_l[i];
				((int16_t *)pcm.data)[i * 2 + 1] = pcm_r[i];
			}

			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("PCM write error: %s", strerror(errno));

		}

		/* update local state with decoded PCM frames */
		rtp_state_update(&rtp, samples / channels);

#endif

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
#if ENABLE_MPG123
fail_open:
#endif
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

static int a2dp_mpeg_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_mpeg_t *caps = capabilities;
	const a2dp_mpeg_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_mpeg_caps_intersect(caps, &sep->config.capabilities);

	if (caps->layer & MPEG_LAYER_MP3)
		caps->layer = MPEG_LAYER_MP3;
	else if (caps->layer & MPEG_LAYER_MP2)
		caps->layer = MPEG_LAYER_MP2;
	else if (caps->layer & MPEG_LAYER_MP1)
		caps->layer = MPEG_LAYER_MP1;
	else {
		error("MPEG: No supported layers: %#x", saved.layer);
		return errno = ENOTSUP, -1;
	}

	unsigned int channel_mode = 0;
	if (a2dp_mpeg_caps_foreach_channel_mode(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode) != -1)
		caps->channel_mode = channel_mode;
	else {
		error("MPEG: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	unsigned int sampling_freq = 0;
	if (a2dp_mpeg_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		caps->sampling_freq = sampling_freq;
	else {
		error("MPEG: No supported sample rates: %#x", saved.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	/* do not waste bits for CRC protection */
	caps->crc = 0;
	/* do not use MPF-2 */
	caps->mpf = 0;

	return 0;
}

static int a2dp_mpeg_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_mpeg_t *conf = configuration;
	a2dp_mpeg_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_mpeg_caps_intersect(&conf_v, &sep->config.capabilities);

	switch (conf_v.layer) {
	case MPEG_LAYER_MP1:
	case MPEG_LAYER_MP2:
	case MPEG_LAYER_MP3:
		break;
	default:
		debug("MPEG: Invalid layer: %#x", conf->layer);
		return A2DP_CHECK_ERR_MPEG_LAYER;
	}

	if (a2dp_bit_mapping_lookup(a2dp_mpeg_channels, conf_v.channel_mode) == -1) {
		debug("MPEG: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	if (a2dp_bit_mapping_lookup(a2dp_mpeg_rates, conf_v.sampling_freq) == -1) {
		debug("MPEG: Invalid sample rate: %#x", conf->sampling_freq);
		return A2DP_CHECK_ERR_RATE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_mpeg_transport_init(struct ba_transport *t) {

	ssize_t channels_i;
	if ((channels_i = a2dp_bit_mapping_lookup(a2dp_mpeg_channels,
					t->media.configuration.mpeg.channel_mode)) == -1)
		return -1;

	ssize_t rate_i;
	if ((rate_i = a2dp_bit_mapping_lookup(a2dp_mpeg_rates,
					t->media.configuration.mpeg.sampling_freq)) == -1)
		return -1;

	t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->media.pcm.channels = a2dp_mpeg_channels[channels_i].value;
	t->media.pcm.rate = a2dp_mpeg_rates[rate_i].value;

	memcpy(t->media.pcm.channel_map, a2dp_mpeg_channels[channels_i].ch.map,
			t->media.pcm.channels * sizeof(*t->media.pcm.channel_map));

	return 0;
}

#if ENABLE_MP3LAME

static int a2dp_mpeg_source_init(struct a2dp_sep *sep) {
	if (config.a2dp.force_mono)
		sep->config.capabilities.mpeg.channel_mode = MPEG_CHANNEL_MODE_MONO;
	if (config.a2dp.force_44100)
		sep->config.capabilities.mpeg.sampling_freq = MPEG_SAMPLING_FREQ_44100;
	return 0;
}

static int a2dp_mpeg_source_transport_start(struct ba_transport *t) {
	if (t->media.configuration.mpeg.layer == MPEG_LAYER_MP3)
		return ba_transport_pcm_start(&t->media.pcm, a2dp_mp3_enc_thread, "ba-a2dp-mp3");
	g_assert_not_reached();
	return -1;
}

struct a2dp_sep a2dp_mpeg_source = {
	.name = "A2DP Source (MP3)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_MPEG12,
		.caps_size = sizeof(a2dp_mpeg_t),
		.capabilities.mpeg = {
			.layer =
				MPEG_LAYER_MP3,
			.crc = 1,
			/* NOTE: LAME does not support dual-channel mode. */
			.channel_mode =
				MPEG_CHANNEL_MODE_MONO |
				MPEG_CHANNEL_MODE_STEREO |
				MPEG_CHANNEL_MODE_JOINT_STEREO,
			/* NOTE: Since MPF-2 is not required for neither Sink
			 *       nor Source, we are not going to support it. */
			.mpf = 0,
			.sampling_freq =
				MPEG_SAMPLING_FREQ_16000 |
				MPEG_SAMPLING_FREQ_22050 |
				MPEG_SAMPLING_FREQ_24000 |
				MPEG_SAMPLING_FREQ_32000 |
				MPEG_SAMPLING_FREQ_44100 |
				MPEG_SAMPLING_FREQ_48000,
			.vbr = 1,
			A2DP_MPEG_INIT_BITRATE(
				MPEG_BITRATE_INDEX_0 |
				MPEG_BITRATE_INDEX_1 |
				MPEG_BITRATE_INDEX_2 |
				MPEG_BITRATE_INDEX_3 |
				MPEG_BITRATE_INDEX_4 |
				MPEG_BITRATE_INDEX_5 |
				MPEG_BITRATE_INDEX_6 |
				MPEG_BITRATE_INDEX_7 |
				MPEG_BITRATE_INDEX_8 |
				MPEG_BITRATE_INDEX_9 |
				MPEG_BITRATE_INDEX_10 |
				MPEG_BITRATE_INDEX_11 |
				MPEG_BITRATE_INDEX_12 |
				MPEG_BITRATE_INDEX_13 |
				MPEG_BITRATE_INDEX_14
			)
		},
	},
	.init = a2dp_mpeg_source_init,
	.configuration_select = a2dp_mpeg_configuration_select,
	.configuration_check = a2dp_mpeg_configuration_check,
	.transport_init = a2dp_mpeg_transport_init,
	.transport_start = a2dp_mpeg_source_transport_start,
	.caps_helpers = &a2dp_mpeg_caps_helpers,
	/* TODO: This is an optional but covered by the A2DP spec codec,
	 *       so it could be enabled by default. However, it does not
	 *       work reliably enough (for now)... */
	.enabled = false,
};

#endif

#if ENABLE_MPG123 || ENABLE_MP3LAME

static int a2dp_mpeg_sink_transport_start(struct ba_transport *t) {
#if ENABLE_MPG123
	return ba_transport_pcm_start(&t->media.pcm, a2dp_mpeg_dec_thread, "ba-a2dp-mpeg");
#else
	if (t->media.configuration.mpeg.layer == MPEG_LAYER_MP3)
		return ba_transport_pcm_start(&t->media.pcm, a2dp_mpeg_dec_thread, "ba-a2dp-mp3");
	g_assert_not_reached();
	return -1;
#endif
}

struct a2dp_sep a2dp_mpeg_sink = {
	.name = "A2DP Sink (MP3)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_MPEG12,
		.caps_size = sizeof(a2dp_mpeg_t),
		.capabilities.mpeg = {
			.layer =
#if ENABLE_MPG123
				MPEG_LAYER_MP1 |
				MPEG_LAYER_MP2 |
#endif
				MPEG_LAYER_MP3,
			.crc = 1,
			/* NOTE: LAME does not support dual-channel mode. Be aware, that
			 *       lack of this feature violates A2DP Sink specification. */
			.channel_mode =
				MPEG_CHANNEL_MODE_MONO |
#if ENABLE_MPG123
				MPEG_CHANNEL_MODE_DUAL_CHANNEL |
#endif
				MPEG_CHANNEL_MODE_STEREO |
				MPEG_CHANNEL_MODE_JOINT_STEREO,
			/* NOTE: Since MPF-2 is not required for neither Sink
			 *       nor Source, we are not going to support it. */
			.mpf = 0,
			.sampling_freq =
				MPEG_SAMPLING_FREQ_16000 |
				MPEG_SAMPLING_FREQ_22050 |
				MPEG_SAMPLING_FREQ_24000 |
				MPEG_SAMPLING_FREQ_32000 |
				MPEG_SAMPLING_FREQ_44100 |
				MPEG_SAMPLING_FREQ_48000,
			.vbr = 1,
			A2DP_MPEG_INIT_BITRATE(
				MPEG_BITRATE_INDEX_0 |
				MPEG_BITRATE_INDEX_1 |
				MPEG_BITRATE_INDEX_2 |
				MPEG_BITRATE_INDEX_3 |
				MPEG_BITRATE_INDEX_4 |
				MPEG_BITRATE_INDEX_5 |
				MPEG_BITRATE_INDEX_6 |
				MPEG_BITRATE_INDEX_7 |
				MPEG_BITRATE_INDEX_8 |
				MPEG_BITRATE_INDEX_9 |
				MPEG_BITRATE_INDEX_10 |
				MPEG_BITRATE_INDEX_11 |
				MPEG_BITRATE_INDEX_12 |
				MPEG_BITRATE_INDEX_13 |
				MPEG_BITRATE_INDEX_14
			)
		},
	},
	.configuration_select = a2dp_mpeg_configuration_select,
	.configuration_check = a2dp_mpeg_configuration_check,
	.transport_init = a2dp_mpeg_transport_init,
	.transport_start = a2dp_mpeg_sink_transport_start,
	.caps_helpers = &a2dp_mpeg_caps_helpers,
	.enabled = false,
};

#endif
