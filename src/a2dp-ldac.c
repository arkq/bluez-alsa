/*
 * BlueALSA - a2dp-ldac.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-ldac.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <glib.h>

#include <ldacBT.h>
#include <ldacBT_abr.h>

#include "a2dp.h"
#include "a2dp-codecs.h"
#include "bluealsa.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

void a2dp_ldac_transport_set_codec(struct ba_transport *t) {

	const struct a2dp_codec *codec = t->a2dp.codec;

	/* LDAC library internally for encoding uses 31-bit integers or
	 * floats, so the best choice for PCM sample is signed 32-bit. */
	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S32_4LE;

	t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
			t->a2dp.configuration.ldac.channel_mode, false);
	t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
			t->a2dp.configuration.ldac.frequency, false);

}

static void *a2dp_ldac_enc_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_LDAC_BT handle;
	if ((handle = ldacBT_get_handle()) == NULL) {
		error("Couldn't get LDAC handle: %s", strerror(errno));
		goto fail_open_ldac;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldacBT_free_handle), handle);

	HANDLE_LDAC_ABR handle_abr;
	if ((handle_abr = ldac_ABR_get_handle()) == NULL) {
		error("Couldn't get LDAC ABR handle: %s", strerror(errno));
		goto fail_open_ldac_abr;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldac_ABR_free_handle), handle_abr);

	const a2dp_ldac_t *configuration = &t->a2dp.configuration.ldac;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t->a2dp.pcm.format);
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
	const size_t ldac_pcm_samples = LDACBT_ENC_LSU * channels;

	if (ldacBT_init_handle_encode(handle, t->mtu_write, config.ldac_eqmid,
				configuration->channel_mode, LDACBT_SMPL_FMT_S32, samplerate) == -1) {
		error("Couldn't initialize LDAC encoder: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
		goto fail_init;
	}

	if (ldac_ABR_Init(handle_abr, 1000 * ldac_pcm_samples / channels / samplerate) == -1) {
		error("Couldn't initialize LDAC ABR");
		goto fail_init;
	}
	if (ldac_ABR_set_thresholds(handle_abr, 6, 4, 2) == -1) {
		error("Couldn't set LDAC ABR thresholds");
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int32_t(&pcm, ldac_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;

	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);
	size_t ts_frames = 0;

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

		int16_t *input = pcm.data;
		size_t input_len = samples;

		/* encode and transfer obtained data */
		while (input_len >= ldac_pcm_samples) {

			/* anchor for RTP payload */
			bt.tail = rtp_payload;

			int used;
			int encoded;
			int frames;

			if (ldacBT_encode(handle, input, &used, bt.tail, &encoded, &frames) != 0) {
				error("LDAC encoding error: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
				break;
			}

			rtp_media_header->frame_count = frames;

			frames = used / sample_size;
			input += frames;
			input_len -= frames;
			ffb_seek(&bt, encoded);

			if (encoded > 0) {

				/* Try to get the number of bytes queued in the
				 * socket output buffer. */
				int queued_bytes = 0;
				if (ioctl(t->bt_fd, TIOCOUTQ, &queued_bytes) != -1)
					queued_bytes = abs(t->a2dp.bt_fd_coutq_init - queued_bytes);

				errno = 0;

				ssize_t len = ffb_blen_out(&bt);
				if ((len = io_bt_write(th, bt.data, len)) <= 0) {
					if (len == -1)
						error("BT write error: %s", strerror(errno));
					goto fail;
				}

				if (errno == EAGAIN)
					/* The io_bt_write() call was blocking due to not enough
					 * space in the BT socket. Set the queued_bytes to some
					 * arbitrary big value. */
					queued_bytes = 1024 * 16;

				if (config.ldac_abr)
					ldac_ABR_Proc(handle, handle_abr, queued_bytes / t->mtu_write, 1);

			}

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, frames / channels);
			ts_frames += frames;

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.asrs) / 100;

			if (encoded > 0) {
				timestamp += ts_frames / channels * 10000 / samplerate;
				rtp_header->seq_number = htobe16(++seq_number);
				rtp_header->timestamp = htobe32(timestamp);
				ts_frames = 0;
			}

		}

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_len);

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open_ldac_abr:
	pthread_cleanup_pop(1);
fail_open_ldac:
	pthread_cleanup_pop(1);
	return NULL;
}

#if HAVE_LDAC_DECODE

static enum ba_transport_thread_signal a2dp_ldac_dec_io_poll_signal_filter(
		enum ba_transport_thread_signal signal, void *userdata) {
	uint16_t *rtp_seq_number = userdata;
	if (signal == BA_TRANSPORT_THREAD_SIGNAL_PCM_CLOSE)
		*rtp_seq_number = 0;
	return signal;
}

static void *a2dp_ldac_dec_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	uint16_t rtp_seq_number = 0;
	struct io_poll io = {
		.signal.filter = a2dp_ldac_dec_io_poll_signal_filter,
		.signal.userdata = &rtp_seq_number,
		.timeout = -1,
	};

	HANDLE_LDAC_BT handle;
	if ((handle = ldacBT_get_handle()) == NULL) {
		error("Couldn't get LDAC handle: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldacBT_free_handle), handle);

	const a2dp_ldac_t *configuration = &t->a2dp.configuration.ldac;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t->a2dp.pcm.format);
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;

	if (ldacBT_init_handle_decode(handle, configuration->channel_mode, samplerate, 0, 0, 0) == -1) {
		error("Couldn't initialize LDAC decoder: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int32_t(&pcm, LDACBT_MAX_LSU * channels) == -1 ||
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

		const rtp_media_header_t *rtp_media_header;
		if ((rtp_media_header = rtp_a2dp_payload(bt.data, &rtp_seq_number)) == NULL)
			continue;

		const uint8_t *rtp_payload = (uint8_t *)(rtp_media_header + 1);
		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		size_t frames = rtp_media_header->frame_count;
		while (frames--) {

			int used;
			int decoded;

			if (ldacBT_decode(handle, (void *)rtp_payload, pcm.data,
						LDACBT_SMPL_FMT_S32, rtp_payload_len, &used, &decoded) != 0) {
				error("LDAC decoding error: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
				break;
			}

			rtp_payload += used;
			rtp_payload_len -= used;

			const size_t samples = decoded / sample_size;
			io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
			if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

		}

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

int a2dp_ldac_transport_start(struct ba_transport *t) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return ba_transport_thread_create(&t->thread_enc, a2dp_ldac_enc_thread, "ba-a2dp-ldac", true);

#if HAVE_LDAC_DECODE
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return ba_transport_thread_create(&t->thread_dec, a2dp_ldac_dec_thread, "ba-a2dp-ldac", true);
#endif

	g_assert_not_reached();
	return -1;
}
