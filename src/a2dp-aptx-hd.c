/*
 * BlueALSA - a2dp-aptx-hd.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-aptx-hd.h"

#if ENABLE_APTX_HD

#include <endian.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "a2dp.h"
#include "a2dp-codecs.h"
#include "codec-aptx.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

void a2dp_aptx_hd_transport_set_codec(struct ba_transport *t) {

	const struct a2dp_codec *codec = t->a2dp.codec;

	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S24_4LE;
	t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
			t->a2dp.configuration.aptx_hd.aptx.channel_mode, false);
	t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
			t->a2dp.configuration.aptx_hd.aptx.frequency, false);

}

static void *a2dp_aptx_hd_enc_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
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

	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
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
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples;
		if ((samples = io_poll_and_read_pcm(&io, &t->a2dp.pcm,
						pcm.tail, ffb_len_in(&pcm))) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
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

			ssize_t len = ffb_blen_out(&bt);
			if ((len = io_bt_write(th, bt.data, len)) <= 0) {
				if (len == -1)
					error("BT write error: %s", strerror(errno));
				goto fail;
			}

			/* keep data transfer at a constant bit rate */
			unsigned int pcm_frames = pcm_samples / channels;
			asrsync_sync(&io.asrs, pcm_frames);
			timestamp += pcm_frames * 10000 / samplerate;

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.asrs) / 100;

			rtp_header->seq_number = htobe16(++seq_number);
			rtp_header->timestamp = htobe32(timestamp);

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
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

#if HAVE_APTX_HD_DECODE

static enum ba_transport_thread_signal a2dp_aptx_hd_dec_io_poll_signal_filter(
		enum ba_transport_thread_signal signal, void *userdata) {
	uint16_t *rtp_seq_number = userdata;
	if (signal == BA_TRANSPORT_THREAD_SIGNAL_PCM_CLOSE)
		*rtp_seq_number = 0;
	return signal;
}

static void *a2dp_aptx_hd_dec_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	uint16_t rtp_seq_number = 0;
	struct io_poll io = {
		.signal.filter = a2dp_aptx_hd_dec_io_poll_signal_filter,
		.signal.userdata = &rtp_seq_number,
		.timeout = -1,
	};

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

	/* Note, that we are allocating space for one extra output packed, which is
	 * required by the aptx_decode_sync() function of libopenaptx library. */
	if (ffb_init_int32_t(&pcm, (t->mtu_read / 6 + 1) * 8) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, th, bt.data, len)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(&t->a2dp.pcm))
			continue;

		const uint8_t *rtp_payload;
		if ((rtp_payload = rtp_a2dp_payload(bt.data, &rtp_seq_number)) == NULL)
			continue;

		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		ffb_rewind(&pcm);
		while (rtp_payload_len >= 6) {

			size_t decoded = ffb_len_in(&pcm);
			ssize_t len;

			if ((len = aptxhddec_decode(handle, rtp_payload, rtp_payload_len, pcm.tail, &decoded)) <= 0) {
				error("Apt-X decoding error: %s", strerror(errno));
				continue;
			}

			rtp_payload += len;
			rtp_payload_len -= len;
			ffb_seek(&pcm, decoded);

		}

		const size_t samples = ffb_len_out(&pcm);
		io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
		if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
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

	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return ba_transport_thread_create(&t->thread_enc, a2dp_aptx_hd_enc_thread, "ba-a2dp-aptx-hd", true);

#if HAVE_APTX_HD_DECODE
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return ba_transport_thread_create(&t->thread_dec, a2dp_aptx_hd_dec_thread, "ba-a2dp-aptx-hd", true);
#endif

	g_assert_not_reached();
	return -1;
}

#endif
