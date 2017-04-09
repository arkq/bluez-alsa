/*
 * BlueALSA - io.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <sbc/sbc.h>
#if ENABLE_AAC
# include <fdk-aac/aacdecoder_lib.h>
# include <fdk-aac/aacenc_lib.h>
# define AACENCODER_LIB_VERSION LIB_VERSION( \
		AACENCODER_LIB_VL0, AACENCODER_LIB_VL1, AACENCODER_LIB_VL2)
#endif

#include "a2dp-codecs.h"
#include "a2dp-rtp.h"
#include "bluealsa.h"
#include "transport.h"
#include "utils.h"
#include "shared/log.h"
#include "shared/rt.h"


/**
 * Wrapper for release callback, which can be used by pthread cleanup. */
static void io_thread_release(struct ba_transport *t) {

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
static int io_thread_open_pcm_read(struct ba_pcm *pcm) {

	/* XXX: This check allows testing. During normal operation PCM FIFO
	 *      should not be opened outside the IO thread function. */
	if (pcm->fd == -1) {
		debug("Opening FIFO for reading: %s", pcm->fifo);
		/* this call will block until writing side is opened */
		if ((pcm->fd = open(pcm->fifo, O_RDONLY)) == -1)
			return -1;
	}

	return 0;
}

/**
 * Open PCM for writing. */
static int io_thread_open_pcm_write(struct ba_pcm *pcm) {

	/* transport PCM FIFO has not been requested */
	if (pcm->fifo == NULL) {
		errno = ENXIO;
		return -1;
	}

	if (pcm->fd == -1) {

		debug("Opening FIFO for writing: %s", pcm->fifo);
		if ((pcm->fd = open(pcm->fifo, O_WRONLY | O_NONBLOCK)) == -1) {
			debug("FIFO reading endpoint is not connected yet");
			return -1;
		}

		/* Restore the blocking mode of our FIFO. Non-blocking mode was required
		 * only for the opening process - we do not want to block if the reading
		 * endpoint is not connected yet. On the other hand, blocking upon data
		 * write will prevent frame dropping. */
		fcntl(pcm->fd, F_SETFL, fcntl(pcm->fd, F_GETFL) & ~O_NONBLOCK);

		/* In order to receive EPIPE while writing to the pipe whose reading end
		 * is closed, the SIGPIPE signal has to be handled. For more information
		 * see the io_thread_write_pcm() function. */
		const struct sigaction sigact = { .sa_handler = SIG_IGN };
		sigaction(SIGPIPE, &sigact, NULL);

	}

	return 0;
}

/**
 * Scale PCM signal according to the transport audio properties. */
static void io_thread_scale_pcm(struct ba_transport *t, int16_t *buffer,
		size_t samples, int channels) {

	/* Get a snapshot of audio properties. Please note, that mutex is not
	 * required here, because we are not modifying these variables. */
	uint8_t ch1_volume = t->a2dp.ch1_volume;
	uint8_t ch2_volume = t->a2dp.ch2_volume;

	double ch1_scale = 0;
	double ch2_scale = 0;

	if (!t->a2dp.ch1_muted)
		ch1_scale = pow(10, (-64 + 64.0 * ch1_volume / 127 ) / 20);
	if (!t->a2dp.ch2_muted)
		ch2_scale = pow(10, (-64 + 64.0 * ch2_volume / 127 ) / 20);

	snd_pcm_scale_s16le(buffer, samples, channels, ch1_scale, ch2_scale);
}

/**
 * Read PCM signal from the transport PCM FIFO. */
static ssize_t io_thread_read_pcm(struct ba_pcm *pcm, int16_t *buffer, size_t samples) {

	uint8_t *head = (uint8_t *)buffer;
	size_t len = samples * sizeof(int16_t);
	ssize_t ret;

	/* This call will block until data arrives. If the passed file descriptor
	 * is invalid (e.g. -1) is means, that other thread (the controller) has
	 * closed the connection. If the connection was closed during the blocking
	 * part, we will still read correct data, because Linux kernel does not
	 * decrement file descriptor reference counter until the read returns. */
	while (len != 0 && (ret = read(pcm->fd, head, len)) != 0) {
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		head += ret;
		len -= ret;
	}

	if (ret > 0)
		/* atomic data read is guaranteed */
		return samples;

	if (ret == 0)
		debug("FIFO endpoint has been closed: %d", pcm->fd);
	if (errno == EBADF)
		ret = 0;
	if (ret == 0)
		transport_release_pcm(pcm);

	return ret;
}

/**
 * Write PCM signal to the transport PCM FIFO. */
static ssize_t io_thread_write_pcm(struct ba_pcm *pcm, const int16_t *buffer, size_t samples) {

	const uint8_t *head = (uint8_t *)buffer;
	size_t len = samples * sizeof(int16_t);
	ssize_t ret;

	do {
		if ((ret = write(pcm->fd, head, len)) == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EPIPE) {
				/* This errno value will be received only, when the SIGPIPE
				 * signal is caught, blocked or ignored. */
				debug("FIFO endpoint has been closed: %d", pcm->fd);
				transport_release_pcm(pcm);
				return 0;
			}
			return ret;
		}
		head += ret;
		len -= ret;
	} while (len != 0);

	/* It is guaranteed, that this function will write data atomically. */
	return samples;
}

/**
 * Convenient wrapper for writing to the RFCOMM socket. */
static ssize_t io_thread_write_rfcomm(int fd, const char *msg) {

	size_t len = strlen(msg);
	ssize_t ret;

retry:
	if ((ret = write(fd, msg, len)) == -1) {
		if (errno == EINTR)
			goto retry;
		error("RFCOMM write error: %s", strerror(errno));
	}

	return ret;
}

/**
 * Write AT command to the RFCOMM. */
static ssize_t io_thread_write_at_command(int fd, const char *msg) {

	char buffer[64];

	snprintf(buffer, sizeof(buffer), "%s\r", msg);
	return io_thread_write_rfcomm(fd, buffer);
}

/**
 * Write AT response code to the RFCOMM. */
static ssize_t io_thread_write_at_response(int fd, const char *msg) {

	char buffer[64];

	snprintf(buffer, sizeof(buffer), "\r\n%s\r\n", msg);
	return io_thread_write_rfcomm(fd, buffer);
}

void *io_thread_a2dp_sink_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	if (t->bt_fd == -1) {
		error("Invalid BT socket: %d", t->bt_fd);
		goto fail_init;
	}

	/* Check for invalid (e.g. not set) reading MTU. If buffer allocation does
	 * not return NULL (allocating zero bytes might return NULL), we will read
	 * zero bytes from the BT socket, which will be wrongly identified as a
	 * "connection closed" action. */
	if (t->mtu_read <= 0) {
		error("Invalid reading MTU: %zu", t->mtu_read);
		goto fail_init;
	}

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->a2dp.cconfig, t->a2dp.cconfig_size)) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);
	const unsigned int channels = transport_get_channels(t);
	uint16_t seq_number = -1;

	const size_t in_buffer_size = t->mtu_read;
	const size_t out_buffer_size = sbc_codesize * (in_buffer_size / sbc_frame_len + 1);
	uint8_t *in_buffer = malloc(in_buffer_size);
	int16_t *out_buffer = malloc(out_buffer_size);

	pthread_cleanup_push(CANCEL_ROUTINE(sbc_finish), &sbc);
	pthread_cleanup_push(CANCEL_ROUTINE(free), in_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), out_buffer);

	if (in_buffer == NULL || out_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ssize_t len;

		/* add BT socket to the poll if transport is active */
		pfds[1].fd = t->state == TRANSPORT_ACTIVE ? t->bt_fd : -1;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);
			continue;
		}

		if ((len = read(pfds[1].fd, in_buffer, in_buffer_size)) == -1) {
			debug("BT read error: %s", strerror(errno));
			continue;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* it seems that zero is never returned... */
		if (len == 0) {
			debug("BT socket has been closed: %d", pfds[1].fd);
			/* Prevent sending the release request to the BlueZ. If the socket has
			 * been closed, it means that BlueZ has already closed the connection. */
			close(pfds[1].fd);
			t->bt_fd = -1;
			goto fail;
		}

		if (io_thread_open_pcm_write(&t->a2dp.pcm) == -1) {
			if (errno != ENXIO)
				error("Couldn't open FIFO: %s", strerror(errno));
			continue;
		}

		const rtp_header_t *rtp_header = (rtp_header_t *)in_buffer;
		const rtp_payload_sbc_t *rtp_payload = (rtp_payload_sbc_t *)&rtp_header->csrc[rtp_header->cc];

		if (rtp_header->paytype != 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}

		uint16_t _seq_number = ntohs(rtp_header->seq_number);
		if (++seq_number != _seq_number) {
			if (seq_number != 0)
				warn("Missing RTP packet: %u != %u", _seq_number, seq_number);
			seq_number = _seq_number;
		}

		const uint8_t *input = (uint8_t *)(rtp_payload + 1);
		int16_t *output = out_buffer;
		size_t input_len = len - (input - in_buffer);
		size_t output_len = out_buffer_size;
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
			output += decoded / sizeof(int16_t);
			output_len -= decoded;
			frames--;

		}

		const size_t samples = output - out_buffer;
		io_thread_scale_pcm(t, out_buffer, samples, channels);
		if (io_thread_write_pcm(&t->a2dp.pcm, out_buffer, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

void *io_thread_a2dp_source_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->a2dp.cconfig, t->a2dp.cconfig_size)) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);
	const unsigned int sbc_frame_duration = sbc_get_frame_duration(&sbc);
	const unsigned int channels = transport_get_channels(t);

	/* Writing MTU should be big enough to contain RTP header, SBC payload
	 * header and at least one SBC frame. In general, there is no constraint
	 * for the MTU value, but the speed might suffer significantly. */
	size_t mtu_write = t->mtu_write;
	if (mtu_write < sizeof(rtp_header_t) + sizeof(rtp_payload_sbc_t) + sbc_frame_len) {
		mtu_write = sizeof(rtp_header_t) + sizeof(rtp_payload_sbc_t) + sbc_frame_len;
		warn("Writing MTU too small for one single SBC frame: %zu < %zu", t->mtu_write, mtu_write);
	}

	const size_t in_buffer_size = sbc_codesize * (mtu_write / sbc_frame_len);
	const size_t out_buffer_size = mtu_write;
	int16_t *in_buffer = malloc(in_buffer_size);
	uint8_t *out_buffer = malloc(out_buffer_size);

	pthread_cleanup_push(CANCEL_ROUTINE(sbc_finish), &sbc);
	pthread_cleanup_push(CANCEL_ROUTINE(free), in_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), out_buffer);

	if (in_buffer == NULL || out_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	if (io_thread_open_pcm_read(&t->a2dp.pcm) == -1) {
		error("Couldn't open FIFO: %s", strerror(errno));
		goto fail;
	}

	uint16_t seq_number = random();
	uint32_t timestamp = random();

	/* initialize RTP header (the constant part) */
	rtp_header_t *rtp_header = (rtp_header_t *)out_buffer;
	memset(rtp_header, 0, sizeof(*rtp_header));
	rtp_header->version = 2;
	rtp_header->paytype = 96;

	rtp_payload_sbc_t *rtp_payload;
	rtp_payload = (rtp_payload_sbc_t *)&rtp_header->csrc[rtp_header->cc];
	memset(rtp_payload, 0, sizeof(*rtp_payload));

	/* reading head position and available read length */
	int16_t *in_buffer_head = in_buffer;
	size_t in_samples = in_buffer_size / sizeof(int16_t);

	struct asrsync asrs = { .frames = 0 };
	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ssize_t samples;

		/* add PCM socket to the poll if transport is active */
		pfds[1].fd = t->state == TRANSPORT_ACTIVE ? t->a2dp.pcm.fd : -1;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);
			asrs.frames = 0;
			continue;
		}

		/* read data from the FIFO - this function will block */
		if ((samples = io_thread_read_pcm(&t->a2dp.pcm, in_buffer_head, in_samples)) <= 0) {
			if (samples == -1)
				error("FIFO read error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* When the thread is created, there might be no data in the FIFO. In fact
		 * there might be no data for a long time - until client starts playback.
		 * In order to correctly calculate time drift, the zero time point has to
		 * be obtained after the stream has started. */
		if (asrs.frames == 0)
			asrsync_init(asrs, transport_get_sampling(t));

		if (!config.a2dp_volume)
			/* scale volume or mute audio signal */
			io_thread_scale_pcm(t, in_buffer_head, samples, channels);

		/* overall input buffer size */
		samples += in_buffer_head - in_buffer;

		const uint8_t *input = (uint8_t *)in_buffer;
		size_t input_len = samples * sizeof(int16_t);

		/* encode and transfer obtained data */
		while (input_len >= sbc_codesize) {

			uint8_t *output = (uint8_t *)(rtp_payload + 1);
			size_t output_len = out_buffer_size - (output - out_buffer);
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
				pcm_frames += len / channels / sizeof(int16_t);
				sbc_frames++;

			}

			rtp_header->seq_number = htons(++seq_number);
			rtp_header->timestamp = htonl(timestamp);
			rtp_payload->frame_count = sbc_frames;

			if (write(t->bt_fd, out_buffer, output - out_buffer) == -1) {
				if (errno == ECONNRESET || errno == ENOTCONN) {
					/* exit the thread upon BT socket disconnection */
					debug("BT socket disconnected");
					goto fail;
				}
				error("BT socket write error: %s", strerror(errno));
			}

			/* keep data transfer at a constant bit rate, also
			 * get a timestamp for the next RTP frame */
			asrsync_sync(&asrs, pcm_frames);
			timestamp += sbc_frame_duration;
			t->delay = asrs.ts_busy.tv_nsec / 100000;

		}

		/* convert bytes length to samples length */
		samples = input_len / sizeof(int16_t);

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		if (samples > 0 && (uint8_t *)in_buffer != input)
			memmove(in_buffer, input, samples * sizeof(int16_t));
		/* reposition our reading head */
		in_buffer_head = in_buffer + samples;
		in_samples = in_buffer_size / sizeof(int16_t) - samples;

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

#if ENABLE_AAC
void *io_thread_a2dp_sink_aac(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	if (t->bt_fd == -1) {
		error("Invalid BT socket: %d", t->bt_fd);
		goto fail_open;
	}
	if (t->mtu_read <= 0) {
		error("Invalid reading MTU: %zu", t->mtu_read);
		goto fail_open;
	}

	HANDLE_AACDECODER handle;
	AAC_DECODER_ERROR err;

	if ((handle = aacDecoder_Open(TT_MP4_LATM_MCP1, 1)) == NULL) {
		error("Couldn't open AAC decoder");
		goto fail_open;
	}

	pthread_cleanup_push(CANCEL_ROUTINE(aacDecoder_Close), handle);

	const unsigned int channels = transport_get_channels(t);
#ifdef AACDECODER_LIB_VL0
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_MIN_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set min output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_MAX_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set max output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
#else
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
#endif

	uint16_t seq_number = -1;

	const size_t in_buffer_size = t->mtu_read;
	const size_t out_buffer_size = 2048 * channels * sizeof(INT_PCM);
	uint8_t *in_buffer = malloc(in_buffer_size);
	int16_t *out_buffer = malloc(out_buffer_size);

	pthread_cleanup_push(CANCEL_ROUTINE(free), in_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), out_buffer);

	if (in_buffer == NULL || out_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		CStreamInfo *aacinf;
		ssize_t len;

		/* add BT socket to the poll if transport is active */
		pfds[1].fd = t->state == TRANSPORT_ACTIVE ? t->bt_fd : -1;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);
			continue;
		}

		if ((len = read(pfds[1].fd, in_buffer, in_buffer_size)) == -1) {
			debug("BT read error: %s", strerror(errno));
			continue;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* it seems that zero is never returned... */
		if (len == 0) {
			debug("BT socket has been closed: %d", pfds[1].fd);
			/* Prevent sending the release request to the BlueZ. If the socket has
			 * been closed, it means that BlueZ has already closed the connection. */
			close(pfds[1].fd);
			t->bt_fd = -1;
			goto fail;
		}

		if (io_thread_open_pcm_write(&t->a2dp.pcm) == -1) {
			if (errno != ENXIO)
				error("Couldn't open FIFO: %s", strerror(errno));
			continue;
		}

		const rtp_header_t *rtp_header = (rtp_header_t *)in_buffer;
		uint8_t *rtp_latm = (uint8_t *)&rtp_header->csrc[rtp_header->cc];
		size_t rtp_latm_len = len - ((void *)rtp_latm - (void *)rtp_header);

		if (rtp_header->paytype != 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}

		uint16_t _seq_number = ntohs(rtp_header->seq_number);
		if (++seq_number != _seq_number) {
			if (seq_number != 0)
				warn("Missing RTP packet: %u != %u", _seq_number, seq_number);
			seq_number = _seq_number;
		}

		unsigned int data_len = rtp_latm_len;
		unsigned int valid = rtp_latm_len;

		if ((err = aacDecoder_Fill(handle, &rtp_latm, &data_len, &valid)) != AAC_DEC_OK)
			error("AAC buffer fill error: %s", aacdec_strerror(err));
		else if ((err = aacDecoder_DecodeFrame(handle, out_buffer, out_buffer_size, 0)) != AAC_DEC_OK)
			error("AAC decode frame error: %s", aacdec_strerror(err));
		else if ((aacinf = aacDecoder_GetStreamInfo(handle)) == NULL)
			error("Couldn't get AAC stream info");
		else {
			const size_t samples = aacinf->frameSize * aacinf->numChannels;
			io_thread_scale_pcm(t, out_buffer, samples, channels);
			if (io_thread_write_pcm(&t->a2dp.pcm, out_buffer, samples) == -1)
				error("FIFO write error: %s", strerror(errno));
		}

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_AAC
void *io_thread_a2dp_source_aac(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;
	const a2dp_aac_t *cconfig = (a2dp_aac_t *)t->a2dp.cconfig;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	HANDLE_AACENCODER handle;
	AACENC_InfoStruct aacinf;
	AACENC_ERROR err;

	/* create AAC encoder without the Meta Data module */
	const unsigned int channels = transport_get_channels(t);
	if ((err = aacEncOpen(&handle, 0x07, channels)) != AACENC_OK) {
		error("Couldn't open AAC encoder: %s", aacenc_strerror(err));
		goto fail_open;
	}

	pthread_cleanup_push(CANCEL_ROUTINE(aacEncClose), &handle);

	unsigned int aot = AOT_NONE;
	unsigned int bitrate = AAC_GET_BITRATE(*cconfig);
	unsigned int samplerate = transport_get_sampling(t);
	unsigned int channelmode = channels == 1 ? MODE_1 : MODE_2;

	switch (cconfig->object_type) {
	case AAC_OBJECT_TYPE_MPEG2_AAC_LC:
#if AACENCODER_LIB_VERSION <= 0x03040C00 /* 3.4.12 */
		aot = AOT_MP2_AAC_LC;
		break;
#endif
	case AAC_OBJECT_TYPE_MPEG4_AAC_LC:
		aot = AOT_AAC_LC;
		break;
	case AAC_OBJECT_TYPE_MPEG4_AAC_LTP:
		aot = AOT_AAC_LTP;
		break;
	case AAC_OBJECT_TYPE_MPEG4_AAC_SCA:
		aot = AOT_AAC_SCAL;
		break;
	}

	if ((err = aacEncoder_SetParam(handle, AACENC_AOT, aot)) != AACENC_OK) {
		error("Couldn't set audio object type: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_BITRATE, bitrate)) != AACENC_OK) {
		error("Couldn't set bitrate: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_SAMPLERATE, samplerate)) != AACENC_OK) {
		error("Couldn't set sampling rate: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_CHANNELMODE, channelmode)) != AACENC_OK) {
		error("Couldn't set channel mode: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if (cconfig->vbr) {
		if ((err = aacEncoder_SetParam(handle, AACENC_BITRATEMODE, config.aac_vbr_mode)) != AACENC_OK) {
			error("Couldn't set VBR bitrate mode %u: %s", config.aac_vbr_mode, aacenc_strerror(err));
			goto fail_init;
		}
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_AFTERBURNER, config.aac_afterburner)) != AACENC_OK) {
		error("Couldn't enable afterburner: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_TRANSMUX, TT_MP4_LATM_MCP1)) != AACENC_OK) {
		error("Couldn't enable LATM transport type: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_HEADER_PERIOD, 1)) != AACENC_OK) {
		error("Couldn't set LATM header period: %s", aacenc_strerror(err));
		goto fail_init;
	}

	if ((err = aacEncEncode(handle, NULL, NULL, NULL, NULL)) != AACENC_OK) {
		error("Couldn't initialize AAC encoder: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncInfo(handle, &aacinf)) != AACENC_OK) {
		error("Couldn't get encoder info: %s", aacenc_strerror(err));
		goto fail_init;
	}

	int in_buffer_identifier = IN_AUDIO_DATA;
	int out_buffer_identifier = OUT_BITSTREAM_DATA;
	int in_buffer_element_size = sizeof(int16_t);
	int out_buffer_element_size = 1;
	int16_t *in_buffer, *in_buffer_head;
	uint8_t *out_buffer, *out_payload;
	int in_buffer_size;
	int out_payload_size;

	AACENC_BufDesc in_buf = {
		.numBufs = 1,
		.bufs = (void **)&in_buffer_head,
		.bufferIdentifiers = &in_buffer_identifier,
		.bufSizes = &in_buffer_size,
		.bufElSizes = &in_buffer_element_size,
	};
	AACENC_BufDesc out_buf = {
		.numBufs = 1,
		.bufs = (void **)&out_payload,
		.bufferIdentifiers = &out_buffer_identifier,
		.bufSizes = &out_payload_size,
		.bufElSizes = &out_buffer_element_size,
	};
	AACENC_InArgs in_args = { 0 };
	AACENC_OutArgs out_args = { 0 };

	in_buffer_size = in_buffer_element_size * aacinf.inputChannels * aacinf.frameLength;
	out_payload_size = aacinf.maxOutBufBytes;
	in_buffer = malloc(in_buffer_size);
	out_buffer = malloc(sizeof(rtp_header_t) + out_payload_size);

	pthread_cleanup_push(CANCEL_ROUTINE(free), in_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), out_buffer);

	if (in_buffer == NULL || out_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	uint16_t seq_number = random();
	uint32_t timestamp = random();

	/* initialize RTP header (the constant part) */
	rtp_header_t *rtp_header = (rtp_header_t *)out_buffer;
	memset(rtp_header, 0, sizeof(*rtp_header));
	rtp_header->version = 2;
	rtp_header->paytype = 96;

	/* anchor for RTP payload - audioMuxElement (RFC 3016) */
	out_payload = (uint8_t *)&rtp_header->csrc[rtp_header->cc];
	/* helper variable used during payload fragmentation */
	const size_t rtp_header_len = out_payload - out_buffer;

	if (io_thread_open_pcm_read(&t->a2dp.pcm) == -1) {
		error("Couldn't open FIFO: %s", strerror(errno));
		goto fail;
	}

	/* initial input buffer head position and the available size */
	size_t in_samples = in_buffer_size / in_buffer_element_size;
	in_buffer_head = in_buffer;

	struct asrsync asrs = { .frames = 0 };
	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ssize_t samples;

		/* add PCM socket to the poll if transport is active */
		pfds[1].fd = t->state == TRANSPORT_ACTIVE ? t->a2dp.pcm.fd : -1;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);
			asrs.frames = 0;
			continue;
		}

		/* read data from the FIFO - this function will block */
		if ((samples = io_thread_read_pcm(&t->a2dp.pcm, in_buffer_head, in_samples)) <= 0) {
			if (samples == -1)
				error("FIFO read error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (asrs.frames == 0)
			asrsync_init(asrs, samplerate);

		if (!config.a2dp_volume)
			/* scale volume or mute audio signal */
			io_thread_scale_pcm(t, in_buffer_head, samples, channels);

		/* overall input buffer size */
		samples += in_buffer_head - in_buffer;
		/* in the encoding loop head is used for reading */
		in_buffer_head = in_buffer;

		while ((in_args.numInSamples = samples) != 0) {

			if ((err = aacEncEncode(handle, &in_buf, &out_buf, &in_args, &out_args)) != AACENC_OK)
				error("AAC encoding error: %s", aacenc_strerror(err));

			if (out_args.numOutBytes > 0) {

				size_t payload_len_max = t->mtu_write - rtp_header_len;
				size_t payload_len = out_args.numOutBytes;
				rtp_header->timestamp = htonl(timestamp);

				/* If the size of the RTP packet exceeds writing MTU, the RTP payload
				 * should be fragmented. According to the RFC 3016, fragmentation of
				 * the audioMuxElement requires no extra header - the payload should
				 * be fragmented and spread across multiple RTP packets.
				 *
				 * TODO: Confirm that the fragmentation logic is correct.
				 *
				 * This code has been tested with Jabra Move headset, however the
				 * outcome of this test is not positive. Fragmented packets are not
				 * recognized by the device. */
				for (;;) {

					ssize_t ret;
					size_t len;

					len = payload_len > payload_len_max ? payload_len_max : payload_len;
					rtp_header->markbit = len < payload_len_max;
					rtp_header->seq_number = htons(++seq_number);

					if ((ret = write(t->bt_fd, out_buffer, rtp_header_len + len)) == -1) {
						if (errno == ECONNRESET || errno == ENOTCONN) {
							/* exit the thread upon BT socket disconnection */
							debug("BT socket disconnected");
							goto fail;
						}
						error("BT socket write error: %s", strerror(errno));
						break;
					}

					/* break if the last part of the payload has been written */
					if ((payload_len -= ret - rtp_header_len) == 0)
						break;

					/* move rest of data to the beginning of the payload */
					debug("Payload fragmentation: extra %zd bytes", payload_len);
					memmove(out_payload, out_payload + ret, payload_len);

				}

			}

			/* progress the head position by the number of samples consumed by the
			 * encoder, also adjust the number of samples in the input buffer */
			in_buffer_head += out_args.numInSamples;
			samples -= out_args.numInSamples;

			/* keep data transfer at a constant bit rate, also
			 * get a timestamp for the next RTP frame */
			unsigned int frames = out_args.numInSamples / channels;
			asrsync_sync(&asrs, frames);
			timestamp += frames * 10000 / samplerate;
			t->delay = asrs.ts_busy.tv_nsec / 100000;

		}

		/* move leftovers to the beginning */
		if (samples > 0 && in_buffer != in_buffer_head)
			memmove(in_buffer, in_buffer_head, samples * in_buffer_element_size);
		/* reposition input buffer head */
		in_buffer_head = in_buffer + samples;
		in_samples = in_buffer_size / in_buffer_element_size - samples;

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

void *io_thread_rfcomm(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	uint8_t mic_gain = t->rfcomm.sco->sco.mic_gain;
	uint8_t spk_gain = t->rfcomm.sco->sco.spk_gain;
	char buffer[64];

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ t->bt_fd, POLLIN, 0 },
	};

	debug("Starting RFCOMM loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {

		const char *response = "OK";
		char command[16], value[32];
		ssize_t ret;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */

			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);

			if (mic_gain != t->rfcomm.sco->sco.mic_gain) {
				mic_gain = t->rfcomm.sco->sco.mic_gain;
				debug("Setting microphone gain: %d", mic_gain);
				sprintf(buffer, "+VGM=%d", mic_gain);
				io_thread_write_at_response(pfds[1].fd, buffer);
			}
			if (spk_gain != t->rfcomm.sco->sco.spk_gain) {
				spk_gain = t->rfcomm.sco->sco.spk_gain;
				debug("Setting speaker gain: %d", spk_gain);
				sprintf(buffer, "+VGS=%d", mic_gain);
				io_thread_write_at_response(pfds[1].fd, buffer);
			}

			continue;
		}

		if ((ret = read(pfds[1].fd, buffer, sizeof(buffer))) == -1) {
			switch (errno) {
			case ECONNABORTED:
			case ECONNRESET:
			case ENOTCONN:
			case ETIMEDOUT:
				/* exit the thread upon socket disconnection */
				debug("RFCOMM disconnected: %s", strerror(errno));
				transport_set_state(t, TRANSPORT_ABORTED);
				goto fail;
			default:
				error("RFCOMM read error: %s", strerror(errno));
				continue;
			}
		}

		/* Parse AT command received from the headset. */
		if (sscanf(buffer, "AT%15[^=]=%30s", command, value) != 2) {
			warn("Invalid AT command: %s", buffer);
			continue;
		}

		debug("AT command: %s=%s", command, value);

		if (strcmp(command, "RING") == 0) {
		}
		else if (strcmp(command, "+CKPD") == 0 && atoi(value) == 200) {
		}
		else if (strcmp(command, "+VGM") == 0)
			t->rfcomm.sco->sco.mic_gain = mic_gain = atoi(value);
		else if (strcmp(command, "+VGS") == 0)
			t->rfcomm.sco->sco.spk_gain = spk_gain = atoi(value);
		else if (strcmp(command, "+IPHONEACCEV") == 0) {

			char *ptr = value;
			size_t count = atoi(strsep(&ptr, ","));
			char tmp;

			while (count-- && ptr != NULL)
				switch (tmp = *strsep(&ptr, ",")) {
				case '1':
					if (ptr != NULL)
						t->device->xapl.accev_battery = atoi(strsep(&ptr, ","));
					break;
				case '2':
					if (ptr != NULL)
						t->device->xapl.accev_docked = atoi(strsep(&ptr, ","));
					break;
				default:
					warn("Unsupported IPHONEACCEV key: %c", tmp);
					strsep(&ptr, ",");
				}

		}
		else if (strcmp(command, "+XAPL") == 0) {

			unsigned int vendor, product;
			unsigned int version, features;

			if (sscanf(value, "%x-%x-%u,%u", &vendor, &product, &version, &features) == 4) {
				t->device->xapl.vendor_id = vendor;
				t->device->xapl.product_id = product;
				t->device->xapl.version = version;
				t->device->xapl.features = features;
				response = "+XAPL=BlueALSA,0";
			}
			else {
				warn("Invalid XAPL value: %s", value);
				response = "ERROR";
			}

		}
		else {
			warn("Unsupported AT command: %s=%s", command, value);
			response = "ERROR";
		}

		io_thread_write_at_response(pfds[1].fd, response);
	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	return NULL;
}

void *io_thread_sco(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	/* this buffer has to be bigger than SCO MTU */
	const size_t buffer_size = 512;
	int16_t *buffer = malloc(buffer_size);

	pthread_cleanup_push(CANCEL_ROUTINE(free), buffer);

	if (buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	struct asrsync asrs = { .frames = 0 };
	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {

		pfds[1].fd = t->sco.mic_pcm.fd != -1 ? t->bt_fd : -1;
		pfds[2].fd = t->sco.spk_pcm.fd;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */

			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);

			/* Try to open reading and/or writing PCM file descriptor. Note,
			 * that we are not checking for errors, because we don't care. */
			io_thread_open_pcm_read(&t->sco.spk_pcm);
			io_thread_open_pcm_write(&t->sco.mic_pcm);

			/* It is required to release SCO if we are not transferring audio,
			 * because it will free Bluetooth bandwidth - microphone signal is
			 * transfered even though we are not reading from it! */
			if (t->sco.spk_pcm.fd == -1 && t->sco.mic_pcm.fd == -1) {
				transport_release_bt_sco(t);
				asrs.frames = 0;
			}
			else
				transport_acquire_bt_sco(t);

			continue;
		}

		if (asrs.frames == 0)
			asrsync_init(asrs, transport_get_sampling(t));

		if (pfds[1].revents & POLLIN) {

			ssize_t len;

			if ((len = read(pfds[1].fd, buffer, buffer_size)) == -1) {
				debug("SCO read error: %s", strerror(errno));
				continue;
			}

			write(t->sco.mic_pcm.fd, buffer, len);
		}

		if (pfds[2].revents & POLLIN) {

			ssize_t samples = t->mtu_write / sizeof(int16_t);

			/* read data from the FIFO - this function will block */
			if ((samples = io_thread_read_pcm(&t->sco.spk_pcm, buffer, samples)) <= 0) {
				if (samples == -1)
					error("FIFO read error: %s", strerror(errno));
				continue;
			}

			write(t->bt_fd, buffer, samples * sizeof(int16_t));
		}

		/* keep data transfer at a constant bit rate */
		asrsync_sync(&asrs, 48 / 2);
		t->delay = asrs.ts_busy.tv_nsec / 100000;

	}

fail:
	pthread_cleanup_pop(1);
	return NULL;
}
