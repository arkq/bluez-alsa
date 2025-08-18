/*
 * BlueALSA - sco-msbc.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "sco-msbc.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "codec-msbc.h"
#include "io.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

void *sco_msbc_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };
	const size_t mtu_write = t->mtu_write;

	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);

	if ((errno = -msbc_init(&msbc)) != 0) {
		error("Couldn't initialize mSBC codec: %s", strerror(errno));
		goto fail_msbc;
	}

	const unsigned int sbc_delay_pcm_frames = 73;
	/* Get the total delay introduced by the codec. */
	t_pcm->codec_delay_dms = sbc_delay_pcm_frames * 10000 / t_pcm->rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &msbc.pcm)) {
		case -1:
			if (errno == ESTALE) {
				/* reinitialize mSBC encoder */
				msbc_init(&msbc);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		while (ffb_len_out(&msbc.pcm) >= MSBC_CODESAMPLES) {

			int err;
			if ((err = msbc_encode(&msbc)) < 0) {
				error("mSBC encoding error: %s", msbc_strerror(err));
				break;
			}

			uint8_t *data = msbc.data.data;
			size_t data_len = ffb_blen_out(&msbc.data);

			while (data_len >= mtu_write) {

				ssize_t len;
				if ((len = io_bt_write(t_pcm, data, mtu_write)) <= 0) {
					if (len == -1)
						error("BT write error: %s", strerror(errno));
					goto exit;
				}

				if (!io.initiated) {
					/* Get the delay due to codec processing. */
					t_pcm->processing_delay_dms = asrsync_get_dms_since_last_sync(&io.asrs);
					ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);
					io.initiated = true;
				}

				data += len;
				data_len -= len;

			}

			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, msbc.frames * MSBC_CODESAMPLES);

			/* Move unprocessed data to the front of our linear
			 * buffer and clear the mSBC frame counter. */
			ffb_shift(&msbc.data, ffb_blen_out(&msbc.data) - data_len);
			msbc.frames = 0;

		}

	}

exit:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_msbc:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

void *sco_msbc_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);

	if ((errno = -msbc_init(&msbc)) != 0) {
		error("Couldn't initialize mSBC codec: %s", strerror(errno));
		goto fail_msbc;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len;
		if ((len = io_poll_and_read_bt(&io, t_pcm, &msbc.data)) == -1)
			error("BT poll and read error: %s", strerror(errno));
		else if (len == 0)
			goto exit;

		if (!ba_transport_pcm_is_active(t_pcm)) {
			ffb_rewind(&msbc.data);
			continue;
		}

		int err;
		/* Process data until there is no more mSBC frames to decode. This loop
		 * ensures that for MTU values bigger than the mSBC frame size, the input
		 * buffer will not fill up causing short reads and mSBC frame losses. */
		while ((err = msbc_decode(&msbc)) > 0)
			continue;
		if (err < 0) {
			error("mSBC decoding error: %s", msbc_strerror(err));
			continue;
		}

		ssize_t samples;
		if ((samples = ffb_len_out(&msbc.pcm)) <= 0)
			continue;

		io_pcm_scale(t_pcm, msbc.pcm.data, samples);
		if ((samples = io_pcm_write(t_pcm, msbc.pcm.data, samples)) == -1)
			error("PCM write error: %s", strerror(errno));
		else if (samples == 0)
			ba_transport_stop_if_no_clients(t);

		ffb_shift(&msbc.pcm, samples);

	}

exit:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_msbc:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
