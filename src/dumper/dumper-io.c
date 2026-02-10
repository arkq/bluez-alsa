/*
 * BlueALSA - dumper-io.c
 * SPDX-FileCopyrightText: 2021-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "dumper.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "io.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"

void * ba_dumper_io_thread(struct ba_transport_pcm * t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport * t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };
	ffb_t bt = { 0 };

	char fname[256];
	snprintf(fname, sizeof(fname), "/tmp/bluealsa-%s-%s.txt",
			ba_transport_to_string(t), ba_transport_pcm_to_string(t_pcm));

	FILE * f;
	debug("Creating BT dump file: %s", fname);
	if ((f = fopen(fname, "w")) == NULL) {
		error("Couldn't create BT dump file: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(fclose), f);

	if (ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_ffb;
	}

	if (ba_dumper_write_header(f, t) == -1) {
		error("Couldn't write header to dump file: %s", strerror(errno));
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

		hexdump("BT data", bt.data, len);
		ba_dumper_write(f, bt.data, len);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
