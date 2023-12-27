/*
 * BlueALSA - a2dp-mpeg.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-mpeg.h"
/* IWYU pragma: no_include "config.h" */

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

static const struct a2dp_channel_mode a2dp_mpeg_channels[] = {
	{ A2DP_CHM_MONO, 1, MPEG_CHANNEL_MODE_MONO },
	{ A2DP_CHM_DUAL_CHANNEL, 2, MPEG_CHANNEL_MODE_DUAL_CHANNEL },
	{ A2DP_CHM_STEREO, 2, MPEG_CHANNEL_MODE_STEREO },
	{ A2DP_CHM_JOINT_STEREO, 2, MPEG_CHANNEL_MODE_JOINT_STEREO },
};

static const struct a2dp_sampling_freq a2dp_mpeg_samplings[] = {
	{ 16000, MPEG_SAMPLING_FREQ_16000 },
	{ 22050, MPEG_SAMPLING_FREQ_22050 },
	{ 24000, MPEG_SAMPLING_FREQ_24000 },
	{ 32000, MPEG_SAMPLING_FREQ_32000 },
	{ 44100, MPEG_SAMPLING_FREQ_44100 },
	{ 48000, MPEG_SAMPLING_FREQ_48000 },
};

struct a2dp_codec a2dp_mpeg_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_MPEG12,
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
		.frequency =
			MPEG_SAMPLING_FREQ_16000 |
			MPEG_SAMPLING_FREQ_22050 |
			MPEG_SAMPLING_FREQ_24000 |
			MPEG_SAMPLING_FREQ_32000 |
			MPEG_SAMPLING_FREQ_44100 |
			MPEG_SAMPLING_FREQ_48000,
		.vbr = 1,
		MPEG_INIT_BITRATE(
			MPEG_BIT_RATE_INDEX_0 |
			MPEG_BIT_RATE_INDEX_1 |
			MPEG_BIT_RATE_INDEX_2 |
			MPEG_BIT_RATE_INDEX_3 |
			MPEG_BIT_RATE_INDEX_4 |
			MPEG_BIT_RATE_INDEX_5 |
			MPEG_BIT_RATE_INDEX_6 |
			MPEG_BIT_RATE_INDEX_7 |
			MPEG_BIT_RATE_INDEX_8 |
			MPEG_BIT_RATE_INDEX_9 |
			MPEG_BIT_RATE_INDEX_10 |
			MPEG_BIT_RATE_INDEX_11 |
			MPEG_BIT_RATE_INDEX_12 |
			MPEG_BIT_RATE_INDEX_13 |
			MPEG_BIT_RATE_INDEX_14
		)
	},
	.capabilities_size = sizeof(a2dp_mpeg_t),
	.channels[0] = a2dp_mpeg_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_mpeg_channels),
	.samplings[0] = a2dp_mpeg_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_mpeg_samplings),
	/* TODO: This is an optional but covered by the A2DP spec codec,
	 *       so it could be enabled by default. However, it does not
	 *       work reliably enough (for now)... */
	.enabled = false,
};

struct a2dp_codec a2dp_mpeg_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_MPEG12,
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
		.frequency =
			MPEG_SAMPLING_FREQ_16000 |
			MPEG_SAMPLING_FREQ_22050 |
			MPEG_SAMPLING_FREQ_24000 |
			MPEG_SAMPLING_FREQ_32000 |
			MPEG_SAMPLING_FREQ_44100 |
			MPEG_SAMPLING_FREQ_48000,
		.vbr = 1,
		MPEG_INIT_BITRATE(
			MPEG_BIT_RATE_INDEX_0 |
			MPEG_BIT_RATE_INDEX_1 |
			MPEG_BIT_RATE_INDEX_2 |
			MPEG_BIT_RATE_INDEX_3 |
			MPEG_BIT_RATE_INDEX_4 |
			MPEG_BIT_RATE_INDEX_5 |
			MPEG_BIT_RATE_INDEX_6 |
			MPEG_BIT_RATE_INDEX_7 |
			MPEG_BIT_RATE_INDEX_8 |
			MPEG_BIT_RATE_INDEX_9 |
			MPEG_BIT_RATE_INDEX_10 |
			MPEG_BIT_RATE_INDEX_11 |
			MPEG_BIT_RATE_INDEX_12 |
			MPEG_BIT_RATE_INDEX_13 |
			MPEG_BIT_RATE_INDEX_14
		)
	},
	.capabilities_size = sizeof(a2dp_mpeg_t),
	.channels[0] = a2dp_mpeg_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_mpeg_channels),
	.samplings[0] = a2dp_mpeg_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_mpeg_samplings),
	.enabled = false,
};

void a2dp_mpeg_init(void) {

	if (config.a2dp.force_mono)
		a2dp_mpeg_source.capabilities.mpeg.channel_mode = MPEG_CHANNEL_MODE_MONO;
	if (config.a2dp.force_44100)
		a2dp_mpeg_source.capabilities.mpeg.frequency = MPEG_SAMPLING_FREQ_44100;

}

void a2dp_mpeg_transport_init(struct ba_transport *t) {

	const struct a2dp_codec *codec = t->a2dp.codec;

	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
			t->a2dp.configuration.mpeg.channel_mode, false);
	t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
			t->a2dp.configuration.mpeg.frequency, false);

}

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

	const a2dp_mpeg_t *configuration = &t->a2dp.configuration.mpeg;
	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;
	MPEG_mode mode = NOT_SET;

	lame_set_num_channels(handle, channels);
	lame_set_in_samplerate(handle, samplerate);

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
		int mpeg_bitrate = MPEG_GET_BITRATE(*configuration);
		int bitrate = a2dp_mpeg1_mp3_get_max_bitrate(mpeg_bitrate);
		if (lame_set_brate(handle, bitrate) != 0) {
			error("LAME: Couldn't set CBR bitrate: %d", bitrate);
			goto fail_setup;
		}
		if (mpeg_bitrate & MPEG_BIT_RATE_FREE &&
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

	const size_t mpeg_pcm_samples = lame_get_framesize(handle);
	const size_t rtp_headers_len = RTP_HEADER_LEN + sizeof(rtp_mpeg_audio_header_t);
	/* It is hard to tell the size of the buffer required, but empirical test
	 * shows that 2KB should be sufficient for encoding. However, encoder flush
	 * function requires a little bit more space. */
	const size_t mpeg_frame_len = 4 * 1024;

	if (ffb_init_int16_t(&pcm, mpeg_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, rtp_headers_len + mpeg_frame_len) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	rtp_mpeg_audio_header_t *rtp_mpeg_audio_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_mpeg_audio_header, sizeof(*rtp_mpeg_audio_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to 90kHz */
	rtp_state_init(&rtp, samplerate, 90000);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t samples = ffb_len_in(&pcm);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, pcm.tail, samples)) {
		case -1:
			if (errno == ESTALE) {
				lame_encode_flush(handle, rtp_payload, mpeg_frame_len);
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

		size_t pcm_frames = samples / channels;
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

		/* keep data transfer at a constant bit rate */
		asrsync_sync(&io.asrs, pcm_frames);
		/* move forward RTP timestamp clock */
		rtp_state_update(&rtp, pcm_frames);

		/* update busy delay (encoding overhead) */
		t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

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

#if ENABLE_MP3LAME || ENABLE_MPG123
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
	const unsigned int samplerate = t_pcm->sampling;

	mpg123_param(handle, MPG123_RESYNC_LIMIT, -1, 0);
	mpg123_param(handle, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
#if MPG123_API_VERSION >= 45
	mpg123_param(handle, MPG123_ADD_FLAGS, MPG123_NO_READAHEAD, 0);
#endif

	mpg123_format_none(handle);
	if (mpg123_format(handle, samplerate, channels, MPG123_ENC_SIGNED_16) != MPG123_OK) {
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
	const unsigned int samplerate = t_pcm->sampling;
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
	rtp_state_init(&rtp, samplerate, 90000);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, bt.data, len)) <= 0) {
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

		long rate;
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
			mpg123_getformat(handle, &rate, &channels_, &encoding);
			debug("MPG123 new format detected: r:%ld, ch:%d, enc:%#x", rate, channels_, encoding);
			break;
		default:
			error("MPG123 decoding error: %s", mpg123_strerror(handle));
			continue;
		}

		const size_t samples = len / sizeof(int16_t);
		io_pcm_scale(t_pcm, pcm.data, samples);
		if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

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
				error("FIFO write error: %s", strerror(errno));
		}
		else {

			ssize_t i;
			for (i = 0; i < samples; i++) {
				((int16_t *)pcm.data)[i * 2 + 0] = pcm_l[i];
				((int16_t *)pcm.data)[i * 2 + 1] = pcm_r[i];
			}

			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

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

int a2dp_mpeg_transport_start(struct ba_transport *t) {

	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE) {
#if ENABLE_MP3LAME
		if (t->a2dp.configuration.mpeg.layer == MPEG_LAYER_MP3)
			return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_mp3_enc_thread, "ba-a2dp-mp3");
#endif
	}

	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SINK) {
#if ENABLE_MPG123
		return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_mpeg_dec_thread, "ba-a2dp-mpeg");
#elif ENABLE_MP3LAME
		if (t->a2dp.configuration.mpeg.layer == MPEG_LAYER_MP3)
			return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_mpeg_dec_thread, "ba-a2dp-mp3");
#endif
	}

	g_assert_not_reached();
	return -1;
}
