/*
 * BlueALSA - sco-cvsd.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "sco-cvsd.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "io.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

void *sco_cvsd_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	const size_t mtu_samples = t->mtu_write / sizeof(int16_t);
	const size_t mtu_write = t->mtu_write;

	ffb_t buffer = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

	/* define a bigger buffer to enhance read performance */
	if (ffb_init_int16_t(&buffer, mtu_samples * 4) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_init;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &buffer)) {
		case -1:
			if (errno == ESTALE)
				continue;
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		const int16_t *input = buffer.data;
		const size_t samples = ffb_len_out(&buffer);
		size_t input_samples = samples;

		while (input_samples >= mtu_samples) {

			ssize_t ret;
			if ((ret = io_bt_write(t_pcm, input, mtu_write)) <= 0) {
				if (ret == -1)
					error("BT write error: %s", strerror(errno));
				goto exit;
			}

			input += mtu_samples;
			input_samples -= mtu_samples;

			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, mtu_samples);

		}

		ffb_shift(&buffer, samples - input_samples);

	}

exit:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_init:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

void *sco_cvsd_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	ffb_t buffer = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

	const size_t mtu_read_multiplier = 3;
	if (ffb_init_uint8_t(&buffer, t->mtu_read * mtu_read_multiplier) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len;
		if ((len = io_poll_and_read_bt(&io, t_pcm, &buffer)) == -1)
			error("BT poll and read error: %s", strerror(errno));
		else if (len == 0)
			goto exit;

		if (!ba_transport_pcm_is_active(t_pcm)) {
			ffb_rewind(&buffer);
			continue;
		}

		ssize_t samples;
		if ((samples = ffb_blen_out(&buffer) / sizeof(int16_t)) <= 0)
			continue;

		io_pcm_scale(t_pcm, buffer.data, samples);
		if ((samples = io_pcm_write(t_pcm, buffer.data, samples)) == -1)
			error("PCM write error: %s", strerror(errno));
		else if (samples == 0)
			ba_transport_stop_if_no_clients(t);

		ffb_shift(&buffer, samples * sizeof(int16_t));

	}

exit:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
