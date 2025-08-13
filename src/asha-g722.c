/*
 * BlueALSA - asha-g722.c
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "asha-g722.h"

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

#include <spandsp.h>

#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "io.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

void * asha_g722_enc_thread(struct ba_transport_pcm * t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport * t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	g722_encode_state_t * state;
	if ((state = g722_encode_init(NULL, 64000, G722_PACKED)) == NULL) {
		error("Couldn't initialize G.722 encoder: %s", strerror(errno));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(g722_encode_free), state);

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const unsigned int channels = t_pcm->channels;
	const size_t g722_frame_pcm_frames = 320;
	const size_t g722_frame_pcm_samples = g722_frame_pcm_frames * channels;

	if (ffb_init_int16_t(&pcm, g722_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	uint8_t seq_number = 0;

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				g722_encode_init(state, 64000, G722_PACKED);
				seq_number = 0;
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		const int16_t * input = pcm.data;
		size_t input_samples = ffb_len_out(&pcm);
		size_t pcm_frames = 0;

		/* Encode and transfer obtained data. */
		while (input_samples >= g722_frame_pcm_samples) {

			((uint8_t *)bt.data)[0] = seq_number++;
			bt.tail = &((uint8_t *)bt.data)[1];

			size_t encoded = g722_encode(state, bt.tail, input, g722_frame_pcm_frames);
			ffb_seek(&bt, encoded);

			input += g722_frame_pcm_samples;
			input_samples -= g722_frame_pcm_samples;
			pcm_frames += g722_frame_pcm_frames;

			ssize_t len = ffb_blen_out(&bt);
			if ((len = io_bt_write(t_pcm, bt.data, len)) <= 0) {
				if (len == -1)
					error("BT write error: %s", strerror(errno));
				goto fail;
			}

			if (!io.initiated) {
				t_pcm->processing_delay_dms = asrsync_get_dms_since_last_sync(&io.asrs);
				ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);
				io.initiated = true;
			}

			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, g722_frame_pcm_frames);

		}

		/* If the input buffer was not consumed (due to codesize limit), we
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
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

__attribute__ ((weak))
void * asha_g722_dec_thread(struct ba_transport_pcm * t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport * t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	g722_decode_state_t * state;
	if ((state = g722_decode_init(NULL, 64000, G722_PACKED)) == NULL) {
		error("Couldn't initialize G.722 decoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(g722_decode_free), state);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const unsigned int channels = t_pcm->channels;
	const size_t g722_frame_pcm_frames = 320;
	const size_t g722_frame_pcm_samples = g722_frame_pcm_frames * channels;

	if (ffb_init_int16_t(&pcm, g722_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	uint8_t seq_number = 0;

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

		uint8_t hdr_seq_number = ((uint8_t *)bt.data)[0];
		const uint8_t * payload = (uint8_t *)bt.data + 1;
		size_t payload_len = len - 1;

		uint8_t missing_asha_frames;
		uint8_t expect_seq_number = seq_number++;
		if ((missing_asha_frames = hdr_seq_number - expect_seq_number) != 0) {
			warn("Missing ASHA packets [%u != %u]: %d",
					hdr_seq_number, expect_seq_number, missing_asha_frames);
			seq_number = hdr_seq_number + 1;
		}

		size_t samples = g722_decode(state, pcm.data, payload, payload_len);

		io_pcm_scale(t_pcm, pcm.data, samples);
		if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
			error("PCM write error: %s", strerror(errno));

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
