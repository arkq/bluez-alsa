/*
 * bluealsa - io.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <sbc/sbc.h>

#include "a2dp-codecs.h"
#include "a2dp-rtp.h"
#include "log.h"
#include "transport.h"


void *io_thread_a2dp_sbc_forward(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->config, t->config_size)) != 0) {
		error("Cannot initialize %s codec: %s", "SBC", strerror(errno));
		return NULL;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);

	const rtp_header_t *rtp_header;
	const size_t rbuffer_size = t->mtu_read;
	const size_t dbuffer_size = sbc_codesize * (rbuffer_size / sbc_frame_len + 1);
	uint8_t *rbuffer;
	uint8_t *dbuffer;

	rbuffer = malloc(rbuffer_size);
	dbuffer = malloc(dbuffer_size);
	rtp_header = (rtp_header_t *)rbuffer;

	if (rbuffer == NULL || dbuffer == NULL) {
		error("Cannot create data buffers: %s", strerror(errno));
		goto fail;
	}

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	if (sigaction(SIGPIPE, &sigact, NULL) == -1)
		warn("Cannot change signal action: %s", strerror(errno));

	struct pollfd pfds[1] = {{ t->bt_fd, POLLIN, 0 }};
	ssize_t len;

	debug("Starting transport IO loop");
	while (t->state == TRANSPORT_ACTIVE) {

		if (poll(pfds, 1, -1) == -1) {
			if (errno != EINTR)
				error("Transport poll error: %s", strerror(errno));
			break;
		}

		if ((len = recv(pfds[0].fd, rbuffer, rbuffer_size, 0)) == 0) {
			debug("BT closed connection: %d", pfds[0].fd);
			break;
		}

		if (len < 0) {
			debug("Data receive failed: %s", strerror(errno));
			continue;
		}

		/* transport PCM FIFO has not been requested */
		if (t->pcm_fifo == NULL)
			continue;

		if (t->pcm_fd == -1) {

			if ((t->pcm_fd = open(t->pcm_fifo, O_WRONLY | O_NONBLOCK)) == -1) {
				if (errno != ENXIO)
					error("Cannot open FIFO: %s", strerror(errno));
				/* FIFO endpoint is not connected yet */
				continue;
			}

			/* Restore the blocking mode of our FIFO. Non-blocking mode was required
			 * only for the opening process - we do not want to block if the reading
			 * endpoint is not connected yet. Blocking upon data write will prevent
			 * frame dropping. */
			fcntl(t->pcm_fd, F_SETFL, fcntl(t->pcm_fd, F_GETFL) ^ O_NONBLOCK);
		}

		if (rtp_header->paytype != 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}

		const uint8_t *input = (uint8_t *)&rtp_header->csrc[rtp_header->cc] + sizeof(rtp_payload_sbc_t);
		size_t input_len = len - (input - rbuffer);
		uint8_t *output = dbuffer;
		size_t output_len = dbuffer_size;

		/* decode retrieved SBC blocks */
		while (input_len > 0) {

			ssize_t len;
			size_t decoded;

			if ((len = sbc_decode(&sbc, input, input_len, output, output_len, &decoded)) <= 0) {
				error("SBC decoding error: %s", strerror(-len));
				break;
			}

			input += len;
			input_len -= len;
			output += decoded;
			output_len -= decoded;
		}

		if (write(t->pcm_fd, dbuffer, dbuffer_size - output_len) == -1) {

			if (errno == EPIPE) {
				/* XXX: When the pipe endpoint is closed it is practically impossible
				 *      to reopen it, since the FIFO node should have been unlinked
				 *      the seconds after the FIFO opening by the client. */

				debug("Closing transport FIFO: %d", t->pcm_fd);
				free(t->pcm_fifo);
				t->pcm_fifo = NULL;
				close(t->pcm_fd);
				t->pcm_fd = -1;

				continue;
			}

			error("Transport FIFO write error: %s", strerror(errno));
		}

	}

fail:

	free(rbuffer);
	free(dbuffer);
	sbc_finish(&sbc);

	debug("Exiting transport IO thread");
	t->state = TRANSPORT_IDLE;
	return NULL;
}

void *io_thread_a2dp_sbc_backward(void *arg) {
	(void)arg;
	return NULL;
}
