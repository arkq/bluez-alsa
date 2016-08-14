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
#include <time.h>
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

	if (t->bt_fd == -1) {
		error("Invalid BT socket: %d", t->bt_fd);
		return NULL;
	}

	/* Check for invalid (e.g. not set) reading MTU. If buffer allocation does
	 * not return NULL (allocating zero bytes might return NULL), we will read
	 * zero bytes from the BT socket, which will be wrongly identified as a
	 * "connection closed" action. */
	if (t->mtu_read <= 0) {
		error("Invalid reading MTU: %zu", t->mtu_read);
		return NULL;
	}

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->config, t->config_size)) != 0) {
		error("Cannot initialize %s codec: %s", "SBC", strerror(errno));
		return NULL;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);

	const size_t rbuffer_size = t->mtu_read;
	const size_t wbuffer_size = sbc_codesize * (rbuffer_size / sbc_frame_len + 1);
	uint8_t *rbuffer = malloc(rbuffer_size);
	uint8_t *wbuffer = malloc(wbuffer_size);

	if (rbuffer == NULL || wbuffer == NULL) {
		error("Cannot create data buffers: %s", strerror(errno));
		goto fail;
	}

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);
	sigaction(SIGUSR1, &sigact, NULL);

	struct pollfd pfds[1] = {{ t->bt_fd, POLLIN, 0 }};
	ssize_t len;

	/* TODO: support for "out of the hat" reading MTU */

	debug("Starting forward IO loop");
	while (t->state == TRANSPORT_ACTIVE) {

		if (poll(pfds, 1, -1) == -1) {
			if (errno != EINTR)
				error("Transport poll error: %s", strerror(errno));
			break;
		}

		if ((len = read(pfds[0].fd, rbuffer, rbuffer_size)) == -1) {
			debug("Data receive failed: %s", strerror(errno));
			continue;
		}

		if (len == 0) {
			debug("BT closed connection: %d", pfds[0].fd);
			break;
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

		const rtp_header_t *rtp_header = (rtp_header_t *)rbuffer;
		const rtp_payload_sbc_t *rtp_payload = (rtp_payload_sbc_t *)&rtp_header->csrc[rtp_header->cc];

		if (rtp_header->paytype != 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}

		const uint8_t *input = (uint8_t *)(rtp_payload + 1);
		uint8_t *output = wbuffer;
		size_t input_len = len - (input - rbuffer);
		size_t output_len = wbuffer_size;
		size_t frames = rtp_payload->frame_count;

		/* decode retrieved SBC frames */
		while (frames && input_len >= sbc_frame_len) {

			ssize_t len;
			size_t decoded;

			if ((len = sbc_decode(&sbc, input, input_len, output, output_len, &decoded)) < 0) {
				error("SBC decoding error: %s", strerror(-len));
				break;
			}

			input += len;
			input_len -= len;
			output += decoded;
			output_len -= decoded;
			frames--;

		}

		if (write(t->pcm_fd, wbuffer, wbuffer_size - output_len) == -1) {

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

	if (t->release != NULL)
		t->release(t);

	if (t->pcm_fd != -1) {
		close(t->pcm_fd);
		t->pcm_fd = -1;
	}

	free(rbuffer);
	free(wbuffer);
	sbc_finish(&sbc);

	debug("Exiting IO thread");
	return NULL;
}

void *io_thread_a2dp_sbc_backward(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->config, t->config_size)) != 0) {
		error("Cannot initialize %s codec: %s", "SBC", strerror(errno));
		return NULL;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);
	const unsigned sbc_frame_duration = sbc_get_frame_duration(&sbc);

	/* Writing MTU should be big enough to contain RTP header, SBC payload
	 * header and at least one SBC frame. In general, there is no constraint
	 * for the MTU value, but the speed might suffer significantly. */
	size_t mtu_write = t->mtu_write;
	if (mtu_write < sizeof(rtp_header_t) + sizeof(rtp_payload_sbc_t) + sbc_frame_len) {
		mtu_write = sizeof(rtp_header_t) + sizeof(rtp_payload_sbc_t) + sbc_frame_len;
		warn("Writing MTU too small for one single SBC frame: %zu < %zu", t->mtu_write, mtu_write);
	}

	rtp_header_t *rtp_header;
	rtp_payload_sbc_t *rtp_payload;
	struct timespec ts0, ts;
	uint16_t seq_number = 0;
	uint32_t timestamp = 0;

	const size_t rbuffer_size = sbc_codesize * (mtu_write / sbc_frame_len);
	const size_t wbuffer_size = mtu_write;
	uint8_t *rbuffer = malloc(rbuffer_size);
	uint8_t *wbuffer = malloc(wbuffer_size);

	if (rbuffer == NULL || wbuffer == NULL) {
		error("Cannot create data buffers: %s", strerror(errno));
		goto fail;
	}

	/* XXX: This check allows testing. During normal operation PCM FIFO
	 *      should not be opened by some external entity. */
	if (t->pcm_fd == -1) {
		debug("Opening FIFO for reading: %s", t->pcm_fifo);
		/* this call will block until writing end is opened */
		if ((t->pcm_fd = open(t->pcm_fifo, O_RDONLY)) == -1) {
			error("Cannot open FIFO: %s", strerror(errno));
			goto fail;
		}
	}

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);
	sigaction(SIGUSR1, &sigact, NULL);

	/* initialize RTP headers (the constant part) */
	rtp_header = (rtp_header_t *)wbuffer;
	memset(rtp_header, 0, sizeof(*rtp_header));
	rtp_payload = (rtp_payload_sbc_t *)&rtp_header->csrc[rtp_header->cc];
	memset(rtp_payload, 0, sizeof(*rtp_payload));
	rtp_header->version = 2;
	rtp_header->paytype = 96;

	struct pollfd pfds[1] = {{ t->pcm_fd, POLLIN, 0 }};
	ssize_t len;

	/* reading head position and available read length */
	uint8_t *rhead = rbuffer;
	size_t rlen = rbuffer_size;

	/* Get initial time point. This time point is used to calculate time drift
	 * during data transmission. The transfer should be kept at constant pace,
	 * so we should be able to detect (and correct) all fluctuations. */
	clock_gettime(CLOCK_MONOTONIC, &ts0);

	debug("Starting backward IO loop");
	while (t->state == TRANSPORT_ACTIVE) {

		if (poll(pfds, 1, -1) == -1) {
			if (errno != EINTR)
				error("Transport poll error: %s", strerror(errno));
			break;
		}

		if ((len = read(pfds[0].fd, rhead, rlen)) == -1) {
			error("FIFO read error: %s", strerror(errno));
			break;
		}

		if (len == 0) {
			debug("FIFO has been closed: %d", pfds[0].fd);
			break;
		}

		const uint8_t *input = rbuffer;
		size_t input_len = rbuffer_size;

		/* encode and transfer obtained data */
		while (input_len >= sbc_codesize) {

			uint8_t *output = (uint8_t *)(rtp_payload + 1);
			size_t output_len = len - (output - wbuffer);
			size_t frames = 0;

			/* Generate as many SBC frames as possible to fill the output buffer
			 * without overflowing it. The size of the output buffer is based on
			 * the socket MTU, so such a transfer should be most efficient. */
			while (input_len >= sbc_codesize && output_len >= sbc_frame_len) {

				ssize_t len;
				ssize_t encoded;

				if ((len = sbc_encode(&sbc, input, input_len, output, output_len, &encoded)) < 0) {
					error("SBC encoding error: %s", strerror(-len));
					break;
				}

				input += len;
				input_len -= len;
				output += encoded;
				output_len -= encoded;
				frames++;

			}

			rtp_header->seq_number = htons(++seq_number);
			rtp_header->timestamp = htonl(timestamp);
			rtp_payload->frame_count = frames;

			/* keep transfer at constant rate, always 10 ms ahead */
			clock_gettime(CLOCK_MONOTONIC, &ts);
			const time_t rt_elapsed = (ts.tv_sec - ts0.tv_sec) * 1e6 + (ts.tv_nsec - ts0.tv_nsec) / 1e3;
			const int rt_delta = timestamp - rt_elapsed - 10e3;
			if (rt_delta > 0)
				usleep(rt_delta);

			if (write(t->bt_fd, wbuffer, output - wbuffer) == -1)
				error("BT socket write error: %s", strerror(errno));

			/* get timestamp for the next frame */
			timestamp += sbc_frame_duration * frames;

		}

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		if (input_len > 0 && rbuffer != input)
			memmove(rbuffer, input, input_len);

		/* reposition our reading head */
		rhead = rbuffer + input_len;
		rlen = rbuffer_size - input_len;

	}

fail:

	if (t->release != NULL)
		t->release(t);

	if (t->pcm_fd != -1) {
		close(t->pcm_fd);
		t->pcm_fd = -1;
	}

	free(rbuffer);
	free(wbuffer);
	sbc_finish(&sbc);

	debug("Exiting IO thread");
	return NULL;
}
