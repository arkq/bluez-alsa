/*
 * BlueALSA - a2dp-sbc.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-sbc.h"
/* IWYU pragma: no_include "config.h" */

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
#include "ba-transport-pcm.h"
#include "bluealsa-config.h"
#include "codec-sbc.h"
#include "io.h"
#include "rtp.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_channel_mode a2dp_sbc_channels[] = {
	{ A2DP_CHM_MONO, 1, SBC_CHANNEL_MODE_MONO },
	{ A2DP_CHM_DUAL_CHANNEL, 2, SBC_CHANNEL_MODE_DUAL_CHANNEL },
	{ A2DP_CHM_STEREO, 2, SBC_CHANNEL_MODE_STEREO },
	{ A2DP_CHM_JOINT_STEREO, 2, SBC_CHANNEL_MODE_JOINT_STEREO },
};

static const struct a2dp_sampling_freq a2dp_sbc_samplings[] = {
	{ 16000, SBC_SAMPLING_FREQ_16000 },
	{ 32000, SBC_SAMPLING_FREQ_32000 },
	{ 44100, SBC_SAMPLING_FREQ_44100 },
	{ 48000, SBC_SAMPLING_FREQ_48000 },
};

struct a2dp_codec a2dp_sbc_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_SBC,
	.capabilities.sbc = {
		.frequency =
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
	.capabilities_size = sizeof(a2dp_sbc_t),
	.channels[0] = a2dp_sbc_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_sbc_channels),
	.samplings[0] = a2dp_sbc_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_sbc_samplings),
	.enabled = true,
};

struct a2dp_codec a2dp_sbc_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_SBC,
	.capabilities.sbc = {
		.frequency =
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
	.capabilities_size = sizeof(a2dp_sbc_t),
	.channels[0] = a2dp_sbc_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_sbc_channels),
	.samplings[0] = a2dp_sbc_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_sbc_samplings),
	.enabled = true,
};

void a2dp_sbc_init(void) {

	if (config.sbc_quality == SBC_QUALITY_XQ ||
			config.sbc_quality == SBC_QUALITY_XQPLUS) {
		info("Activating SBC Dual Channel HD (SBC %s)",
				config.sbc_quality == SBC_QUALITY_XQ ? "XQ" : "XQ+");
		config.a2dp.force_44100 = true;
	}

	if (config.a2dp.force_mono)
		/* With this we are violating A2DP SBC requirements. According to spec,
		 * SBC source shall support mono channel and at least one of the stereo
		 * modes. However, since for sink all channel modes are mandatory, even
		 * though we are supporting only mono mode, there will be a match when
		 * selecting configuration. */
		a2dp_sbc_source.capabilities.sbc.channel_mode = SBC_CHANNEL_MODE_MONO;
	if (config.a2dp.force_44100)
		a2dp_sbc_source.capabilities.sbc.frequency = SBC_SAMPLING_FREQ_44100;

}

void a2dp_sbc_transport_init(struct ba_transport *t) {

	const struct a2dp_codec *codec = t->a2dp.codec;

	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
			t->a2dp.configuration.sbc.channel_mode, false);
	t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
			t->a2dp.configuration.sbc.frequency, false);

}

void *a2dp_sbc_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	sbc_t sbc;
	const a2dp_sbc_t *configuration = &t->a2dp.configuration.sbc;
	if ((errno = -sbc_init_a2dp(&sbc, 0, configuration, sizeof(*configuration))) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);

	const size_t sbc_frame_samples = sbc_get_codesize(&sbc) / sizeof(int16_t);
	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;

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

	size_t ffb_pcm_len = sbc_frame_samples;
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

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;

	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio samplerate */
	rtp_state_init(&rtp, samplerate, samplerate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t samples = ffb_len_in(&pcm);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, pcm.tail, samples)) {
		case -1:
			if (errno == ESTALE) {
				sbc_reinit_a2dp(&sbc, 0, configuration, sizeof(*configuration));
				sbc.bitpool = sbc_a2dp_get_bitpool(configuration, config.sbc_quality);
				sbc.endian = SBC_LE;
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

		const int16_t *input = pcm.data;
		size_t input_samples = samples;
		size_t output_len = ffb_len_in(&bt);
		size_t pcm_frames = 0;
		size_t sbc_frames = 0;

		/* Generate as many SBC frames as possible, but less than a 4-bit media
		 * header frame counter can contain. The size of the output buffer is
		 * based on the socket MTU, so such transfer should be most efficient. */
		while (input_samples >= sbc_frame_samples &&
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
	if ((errno = -sbc_init_a2dp(&sbc, 0, &t->a2dp.configuration.sbc,
					sizeof(t->a2dp.configuration.sbc))) != 0) {
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
	const unsigned int samplerate = t_pcm->sampling;

	if (ffb_init_int16_t(&pcm, sbc_get_codesize(&sbc)) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio samplerate */
	rtp_state_init(&rtp, samplerate, samplerate);

#if DEBUG
	uint16_t sbc_bitpool = 0;
#endif

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
				error("FIFO write error: %s", strerror(errno));

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

int a2dp_sbc_transport_start(struct ba_transport *t) {

	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_sbc_enc_thread, "ba-a2dp-sbc");

	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_sbc_dec_thread, "ba-a2dp-sbc");

	g_assert_not_reached();
	return -1;
}
