/*
 * BlueALSA - a2dp-aptx-hd.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-aptx-hd.h"
/* IWYU pragma: no_include "config.h" */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "a2dp.h"
#include "ba-transport-pcm.h"
#include "codec-aptx.h"
#include "io.h"
#include "rtp.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_channel_mode a2dp_aptx_hd_channels[] = {
	{ A2DP_CHM_STEREO, 2, APTX_CHANNEL_MODE_STEREO },
};

static const struct a2dp_sampling_freq a2dp_aptx_hd_samplings[] = {
	{ 16000, APTX_SAMPLING_FREQ_16000 },
	{ 32000, APTX_SAMPLING_FREQ_32000 },
	{ 44100, APTX_SAMPLING_FREQ_44100 },
	{ 48000, APTX_SAMPLING_FREQ_48000 },
};

struct a2dp_codec a2dp_aptx_hd_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_APTX_HD,
	.capabilities.aptx_hd = {
		.aptx.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
		/* NOTE: Used apt-X HD library does not support
		 *       single channel (mono) mode. */
		.aptx.channel_mode =
			APTX_CHANNEL_MODE_STEREO,
		.aptx.frequency =
			APTX_SAMPLING_FREQ_16000 |
			APTX_SAMPLING_FREQ_32000 |
			APTX_SAMPLING_FREQ_44100 |
			APTX_SAMPLING_FREQ_48000,
	},
	.capabilities_size = sizeof(a2dp_aptx_hd_t),
	.channels[0] = a2dp_aptx_hd_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aptx_hd_channels),
	.samplings[0] = a2dp_aptx_hd_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aptx_hd_samplings),
};

struct a2dp_codec a2dp_aptx_hd_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_APTX_HD,
	.capabilities.aptx_hd = {
		.aptx.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
		/* NOTE: Used apt-X HD library does not support
		 *       single channel (mono) mode. */
		.aptx.channel_mode =
			APTX_CHANNEL_MODE_STEREO,
		.aptx.frequency =
			APTX_SAMPLING_FREQ_16000 |
			APTX_SAMPLING_FREQ_32000 |
			APTX_SAMPLING_FREQ_44100 |
			APTX_SAMPLING_FREQ_48000,
	},
	.capabilities_size = sizeof(a2dp_aptx_hd_t),
	.channels[0] = a2dp_aptx_hd_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_aptx_hd_channels),
	.samplings[0] = a2dp_aptx_hd_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_aptx_hd_samplings),
};

void a2dp_aptx_hd_init(void) {
}

void a2dp_aptx_hd_transport_init(struct ba_transport *t) {

	const struct a2dp_codec *codec = t->a2dp.codec;

	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S24_4LE;
	t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
			t->a2dp.configuration.aptx_hd.aptx.channel_mode, false);
	t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
			t->a2dp.configuration.aptx_hd.aptx.frequency, false);

}

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
	const unsigned int samplerate = t_pcm->sampling;
	const size_t aptx_pcm_samples = 4 * channels;
	const size_t aptx_code_len = 2 * 3 * sizeof(uint8_t);
	const size_t mtu_write = t->mtu_write;

	if (ffb_init_int32_t(&pcm, aptx_pcm_samples * ((mtu_write - RTP_HEADER_LEN) / aptx_code_len)) == -1 ||
			ffb_init_uint8_t(&bt, mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	/* initialize RTP header and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header, NULL, 0);

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio samplerate */
	rtp_state_init(&rtp, samplerate, samplerate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t samples = ffb_len_in(&pcm);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, pcm.tail, samples)) {
		case -1:
			if (errno == ESTALE) {
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

		int32_t *input = pcm.data;
		size_t input_samples = samples;

		/* encode and transfer obtained data */
		while (input_samples >= aptx_pcm_samples) {

			/* anchor for RTP payload */
			bt.tail = rtp_payload;

			size_t output_len = ffb_len_in(&bt);
			size_t pcm_samples = 0;

			/* Generate as many apt-X frames as possible to fill the output buffer
			 * without overflowing it. The size of the output buffer is based on
			 * the socket MTU, so such a transfer should be most efficient. */
			while (input_samples >= aptx_pcm_samples && output_len >= aptx_code_len) {

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

			unsigned int pcm_frames = pcm_samples / channels;
			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp.ts_pcm_frames += pcm_frames;

			/* update busy delay (encoding overhead) */
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

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
	const unsigned int samplerate = t_pcm->sampling;

	/* Note, that we are allocating space for one extra output packed, which is
	 * required by the aptx_decode_sync() function of libopenaptx library. */
	if (ffb_init_int32_t(&pcm, (t->mtu_read / 6 + 1) * 8) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio samplerate */
	rtp_state_init(&rtp, samplerate, samplerate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, bt.data, len)) <= 0) {
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
			error("FIFO write error: %s", strerror(errno));

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

int a2dp_aptx_hd_transport_start(struct ba_transport *t) {

	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_aptx_hd_enc_thread, "ba-a2dp-aptx-hd");

#if HAVE_APTX_HD_DECODE
	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_aptx_hd_dec_thread, "ba-a2dp-aptx-hd");
#endif

	g_assert_not_reached();
	return -1;
}
