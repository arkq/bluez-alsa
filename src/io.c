/*
 * BlueALSA - io.c
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
#include "utils.h"


struct io_sync {

	/* reference time point */
	struct timespec ts0;
	/* transfered frames since ts0 */
	uint32_t frames;
	/* used sampling frequency */
	uint16_t sampling;

};

/**
 * Wrapper for release callback, which can be used by pthread cleanup. */
static void io_thread_release_bt(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	/* During the normal operation mode, the release callback should not
	 * be NULL. Hence, we will relay on this callback - file descriptors
	 * are closed in it. */
	if (t->release != NULL)
		t->release(t);

	/* XXX: If the order of the cleanup push is right, this function will
	 *      indicate the end of the IO thread. */
	debug("Exiting IO thread");
}

/**
 * Open PCM for reading. */
static int io_thread_open_pcm_read(struct ba_transport *t) {

	/* XXX: This check allows testing. During normal operation PCM FIFO
	 *      should not be opened outside the IO thread function. */
	if (t->pcm_fd == -1) {
		debug("Opening FIFO for reading: %s", t->pcm_fifo);
		/* this call will block until writing side is opened */
		if ((t->pcm_fd = open(t->pcm_fifo, O_RDONLY)) == -1)
			return -1;
	}

	return 0;
}

/**
 * Open PCM for writing. */
static int io_thread_open_pcm_write(struct ba_transport *t) {

	/* transport PCM FIFO has not been requested */
	if (t->pcm_fifo == NULL) {
		errno = ENXIO;
		return -1;
	}

	if (t->pcm_fd == -1) {

		debug("Opening FIFO for writing: %s", t->pcm_fifo);
		if ((t->pcm_fd = open(t->pcm_fifo, O_WRONLY | O_NONBLOCK)) == -1)
			/* FIFO endpoint is not connected yet */
			return -1;

		/* Restore the blocking mode of our FIFO. Non-blocking mode was required
		 * only for the opening process - we do not want to block if the reading
		 * endpoint is not connected yet. On the other hand, blocking upon data
		 * write will prevent frame dropping. */
		fcntl(t->pcm_fd, F_SETFL, fcntl(t->pcm_fd, F_GETFL) & ~O_NONBLOCK);

	}

	return 0;
}

/**
 * Scale PCM signal according to the transport audio properties. */
static void io_thread_scale_pcm(struct ba_transport *t, void *buffer, size_t size) {

	/* Get a snapshot of audio properties. Please note, that mutex is not
	 * required here, because we are not modifying these variables. */
	const uint8_t volume = t->volume;
	const uint8_t muted = t->muted;

	if (muted || volume == 0)
		snd_pcm_mute_s16le(buffer, size);
	else if (volume != 100)
		snd_pcm_scale_s16le(buffer, size, volume);

}

/**
 * Pause IO thread until the resume signal is received. */
static void io_thread_pause(struct ba_transport *t) {
	debug("Pausing IO thread: %s", t->name);
	pthread_mutex_lock(&t->resume_mutex);
	pthread_cond_wait(&t->resume, &t->resume_mutex);
	pthread_mutex_unlock(&t->resume_mutex);
	debug("Resuming IO thread: %s", t->name);
}

/**
 * Synchronize thread timing with the audio sampling frequency.
 *
 * Time synchronization relies on the frame counter being linear. This counter
 * should be initialized upon transfer start and resume. With the size of this
 * counter being 32 bits, it is possible to track up to ~24 hours of playback,
 * with the sampling rate of 48 kHz. If this is insufficient, one can switch
 * to 64 bits, which would be sufficient to play for 12 million years. */
static int io_thread_time_sync(struct io_sync *io_sync, uint32_t frames) {

	const uint16_t sampling = io_sync->sampling;
	struct timespec ts_audio;
	struct timespec ts_clock;
	struct timespec ts;

	io_sync->frames += frames;
	frames = io_sync->frames;

	/* keep transfer 10ms ahead */
	unsigned int overframes = sampling / 100;
	frames = frames > overframes ? frames - overframes : 0;

	ts_audio.tv_sec = frames / sampling;
	ts_audio.tv_nsec = 1000000000 / sampling * (frames % sampling);

	clock_gettime(CLOCK_MONOTONIC, &ts_clock);
	difftimespec(&io_sync->ts0, &ts_clock, &ts_clock);

	/* maintain constant bit rate */
	if (difftimespec(&ts_clock, &ts_audio, &ts) > 0) {
		nanosleep(&ts, NULL);
		return 1;
	}

	return 0;
}

void *io_thread_a2dp_sbc_forward(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

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

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->config, t->config_size)) != 0) {
		error("Couldn't initialize %s codec: %s", "SBC", strerror(errno));
		return NULL;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);

	const size_t rbuffer_size = t->mtu_read;
	const size_t wbuffer_size = sbc_codesize * (rbuffer_size / sbc_frame_len + 1);
	uint8_t *rbuffer = malloc(rbuffer_size);
	uint8_t *wbuffer = malloc(wbuffer_size);

	pthread_cleanup_push(io_thread_release_bt, t);
	pthread_cleanup_push(sbc_finish, &sbc);
	pthread_cleanup_push(free, wbuffer);
	pthread_cleanup_push(free, rbuffer);

	if (rbuffer == NULL || wbuffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail;
	}

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	struct pollfd pfds[1] = {{ t->bt_fd, POLLIN, 0 }};

	/* TODO: support for "out of the hat" reading MTU */

	debug("Starting forward IO loop");
	while (TRANSPORT_RUN_IO_THREAD(t)) {

		ssize_t len;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		if (t->state == TRANSPORT_PAUSED)
			io_thread_pause(t);

		if (poll(pfds, 1, -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			break;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if ((len = read(pfds[0].fd, rbuffer, rbuffer_size)) == -1) {
			debug("BT read error: %s", strerror(errno));
			continue;
		}

		/* It seems that this block of code is not executed... */
		if (len == 0) {
			debug("BT socket has been closed: %d", pfds[0].fd);
			/* Prevent sending the release request to the BlueZ. If the socket has
			 * been closed, it means that BlueZ has already closed the connection. */
			close(pfds[0].fd);
			t->bt_fd = -1;
			break;
		}

		if (io_thread_open_pcm_write(t) == -1) {
			if (errno != ENXIO)
				error("Couldn't open FIFO: %s", strerror(errno));
			continue;
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
				debug("FIFO endpoint has been closed: %d", t->pcm_fd);
				transport_release_pcm(t);
				continue;
			}

			error("FIFO write error: %s", strerror(errno));
		}

	}

fail:
	warn("IO thread failure");
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

void *io_thread_a2dp_sbc_backward(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->config, t->config_size)) != 0) {
		error("Couldn't initialize %s codec: %s", "SBC", strerror(errno));
		return NULL;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);
	const unsigned sbc_frame_duration = sbc_get_frame_duration(&sbc);
	const unsigned int channels = transport_get_channels(t);

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
	uint16_t seq_number = 0;
	uint32_t timestamp = 0;

	const size_t rbuffer_size = sbc_codesize * (mtu_write / sbc_frame_len);
	const size_t wbuffer_size = mtu_write;
	uint8_t *rbuffer = malloc(rbuffer_size);
	uint8_t *wbuffer = malloc(wbuffer_size);

	pthread_cleanup_push(io_thread_release_bt, t);
	pthread_cleanup_push(sbc_finish, &sbc);
	pthread_cleanup_push(free, wbuffer);
	pthread_cleanup_push(free, rbuffer);

	if (rbuffer == NULL || wbuffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail;
	}

	if (io_thread_open_pcm_read(t) == -1) {
		error("Couldn't open FIFO: %s", strerror(errno));
		goto fail;
	}

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* initialize RTP headers (the constant part) */
	rtp_header = (rtp_header_t *)wbuffer;
	memset(rtp_header, 0, sizeof(*rtp_header));
	rtp_payload = (rtp_payload_sbc_t *)&rtp_header->csrc[rtp_header->cc];
	memset(rtp_payload, 0, sizeof(*rtp_payload));
	rtp_header->version = 2;
	rtp_header->paytype = 96;

	/* reading head position and available read length */
	uint8_t *rhead = rbuffer;
	size_t rlen = rbuffer_size;

	struct io_sync io_sync = {
		.sampling = transport_get_sampling(t),
	};

	debug("Starting backward IO loop");
	while (TRANSPORT_RUN_IO_THREAD(t)) {

		ssize_t len;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		if (t->state == TRANSPORT_PAUSED) {
			io_thread_pause(t);
			io_sync.frames = 0;
		}

		/* This call will block until data arrives. If the passed file descriptor
		 * is invalid (e.g. -1) is means, that other thread (the controller) has
		 * closed the connection. If the connection was closed during the blocking
		 * part, we will still read correct data, because Linux kernel does not
		 * decrement file descriptor reference counter until the read returns. */
		if ((len = read(t->pcm_fd, rhead, rlen)) == -1) {
			if (errno == EINTR)
				continue;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (len <= 0) {
			if (len == -1 && errno != EBADF)
				error("FIFO read error: %s", strerror(errno));
			else if (len == 0)
				debug("FIFO endpoint has been closed: %d", t->pcm_fd);
			transport_release_pcm(t);
			goto fail;
		}

		/* When the thread is created, there might be no data in the FIFO. In fact
		 * there might be no data for a long time - until client starts playback.
		 * In order to correctly calculate time drift, the zero time point has to
		 * be obtained after the stream has started. */
		if (io_sync.frames == 0)
			clock_gettime(CLOCK_MONOTONIC, &io_sync.ts0);

		const uint8_t *input = rbuffer;
		size_t input_len = (rhead - rbuffer) + len;

		/* lower volume or mute audio signal */
		io_thread_scale_pcm(t, rbuffer, input_len);

		/* encode and transfer obtained data */
		while (input_len >= sbc_codesize) {

			uint8_t *output = (uint8_t *)(rtp_payload + 1);
			size_t output_len = wbuffer_size - (output - wbuffer);
			size_t pcm_frames = 0;
			size_t sbc_frames = 0;

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
				pcm_frames += len / channels / 2;
				sbc_frames++;

			}

			rtp_header->seq_number = htons(++seq_number);
			rtp_header->timestamp = htonl(timestamp);
			rtp_payload->frame_count = sbc_frames;

			if (write(t->bt_fd, wbuffer, output - wbuffer) == -1) {
				if (errno == ECONNRESET || errno == ENOTCONN) {
					/* exit the thread upon BT socket disconnection */
					debug("BT socket disconnected: %s", strerror(errno));
					goto fail;
				}
				error("BT socket write error: %s", strerror(errno));
			}

			/* get a timestamp for the next frame */
			timestamp += sbc_frame_duration * sbc_frames;

			/* keep transfer at a constant bit rate */
			io_thread_time_sync(&io_sync, pcm_frames);

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
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
