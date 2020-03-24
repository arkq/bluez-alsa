/*
 * BlueALSA - a2dp.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp.h"

#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sbc/sbc.h>
#if ENABLE_AAC
# include <fdk-aac/aacdecoder_lib.h>
# include <fdk-aac/aacenc_lib.h>
# define AACENCODER_LIB_VERSION LIB_VERSION( \
		AACENCODER_LIB_VL0, AACENCODER_LIB_VL1, AACENCODER_LIB_VL2)
#endif
#if ENABLE_APTX || ENABLE_APTX_HD
# include <openaptx.h>
#endif
#if ENABLE_MP3LAME
# include <lame/lame.h>
#endif
#if ENABLE_MPG123
# include <mpg123.h>
#endif
#if ENABLE_LDAC
# include <ldacBT.h>
# include <ldacBT_abr.h>
#endif

#include "a2dp-codecs.h"
#include "a2dp-rtp.h"
#include "bluealsa.h"
#include "bluez-a2dp.h"
#include "sbc.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

/**
 * Common IO thread data. */
struct io_thread_data {
	/* keep-alive and sync timeout */
	int timeout;
	/* transfer bit rate synchronization */
	struct asrsync asrs;
	/* history of BT socket COUTQ bytes */
	struct { int v[16]; size_t i; } coutq;
	/* determine whether transport is locked */
	bool t_locked;
};

/**
 * Scale PCM signal according to the volume configuration. */
static void io_thread_scale_pcm(const struct ba_transport_pcm *pcm,
		int16_t *buffer, size_t samples) {

	if (!pcm->soft_volume)
		return;

	double ch1_scale = 0;
	double ch2_scale = 0;

	if (!pcm->volume[0].muted)
		ch1_scale = pow(10, (-64 + 64.0 * pcm->volume[0].level / 127) / 20);
	if (!pcm->volume[1].muted)
		ch2_scale = pow(10, (-64 + 64.0 * pcm->volume[1].level / 127) / 20);

	snd_pcm_scale_s16le(buffer, samples, pcm->channels, ch1_scale, ch2_scale);
}

/**
 * Read PCM signal from the transport PCM FIFO. */
ssize_t io_thread_read_pcm(struct ba_transport_pcm *pcm, int16_t *buffer, size_t samples) {

	ssize_t ret;

	/* If the passed file descriptor is invalid (e.g. -1) is means, that other
	 * thread (the controller) has closed the connection. If the connection was
	 * closed during this call, we will still read correct data, because Linux
	 * kernel does not decrement file descriptor reference counter until the
	 * read returns. */
	while ((ret = read(pcm->fd, buffer, samples * sizeof(int16_t))) == -1 &&
			errno == EINTR)
		continue;

	if (ret > 0)
		return ret / sizeof(int16_t);

	if (ret == 0)
		debug("PCM has been closed: %d", pcm->fd);
	if (errno == EBADF)
		ret = 0;
	if (ret == 0)
		ba_transport_release_pcm(pcm);

	return ret;
}

/**
 * Flush read buffer of the transport PCM FIFO. */
ssize_t io_thread_read_pcm_flush(struct ba_transport_pcm *pcm) {
	ssize_t rv = splice(pcm->fd, NULL, config.null_fd, NULL, 1024 * 32, SPLICE_F_NONBLOCK);
	if (rv == -1 && errno == EAGAIN)
		rv = 0;
	debug("PCM read buffer flushed: %zd", rv >= 0 ? (int)(rv / sizeof(int16_t)) : rv);
	return rv;
}

/**
 * Write PCM signal to the transport PCM FIFO.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
ssize_t io_thread_write_pcm(struct ba_transport_pcm *pcm,
		const int16_t *buffer, size_t samples) {

	struct pollfd pfd = { pcm->fd, POLLOUT, 0 };
	const uint8_t *head = (uint8_t *)buffer;
	size_t len = samples * sizeof(int16_t);
	int oldstate;
	ssize_t ret;

	/* In order to provide a way of escaping from the infinite poll() we have
	 * to temporally re-enable thread cancellation. */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

	do {
		if ((ret = write(pcm->fd, head, len)) == -1)
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
				poll(&pfd, 1, -1);
				continue;
			case EPIPE:
				/* This errno value will be received only, when the SIGPIPE
				 * signal is caught, blocked or ignored. */
				debug("PCM has been closed: %d", pcm->fd);
				ba_transport_release_pcm(pcm);
				ret = 0;
				/* fall-through */
			default:
				goto final;
			}
		head += ret;
		len -= ret;
	} while (len != 0);

	/* It is guaranteed, that this function will write data atomically. */
	ret = samples;

final:
	pthread_setcancelstate(oldstate, NULL);
	return ret;
}

/**
 * Write data to the BT SEQPACKET socket.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
static ssize_t io_thread_write_bt(int fd, const uint8_t *buffer,
		size_t len, int *coutq, int coutq_init) {

	struct pollfd pfd = { fd, POLLOUT, 0 };
	int oldstate;
	ssize_t ret;

	/* BT socket is opened in the non-blocking mode. However, this function
	 * forcefully operates in a blocking mode - it uses poll() when writing
	 * to the BT socket would block. Hence, it is required to provide a way
	 * of escaping from the poll() when the IO thread termination request
	 * has been made by re-enabling thread cancellation. */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

	if (ioctl(pfd.fd, TIOCOUTQ, coutq) == -1)
		warn("Couldn't get BT queued bytes: %s", strerror(errno));
	else
		*coutq = abs(coutq_init - *coutq);

retry:
	if ((ret = write(pfd.fd, buffer, len)) == -1)
		switch (errno) {
		case EINTR:
			goto retry;
		case EAGAIN:
			poll(&pfd, 1, -1);
			/* set coutq to some arbitrary big value */
			*coutq = 1024 * 16;
			goto retry;
		}

	pthread_setcancelstate(oldstate, NULL);
	return ret;
}

/**
 * Poll and read PCM signal from the transport PCM FIFO.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
static ssize_t a2dp_poll_and_read_pcm(struct ba_transport *t,
		struct io_thread_data *io, ffb_int16_t *buffer) {

	struct pollfd fds[2] = {
		{ t->sig_fd[0], POLLIN, 0 },
		{ -1, POLLIN, 0 }};

	/* Allow escaping from the poll() by thread cancellation. */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

repoll:

	/* Add PCM socket to the poll if transport is active. */
	fds[1].fd = t->state == BA_TRANSPORT_STATE_ACTIVE ? t->a2dp.pcm.fd : -1;

	/* Poll for reading with keep-alive and sync timeout. */
	switch (poll(fds, ARRAYSIZE(fds), io->timeout)) {
	case 0:
		pthread_cond_signal(&t->a2dp.pcm.synced);
		io->timeout = -1;
		io->t_locked = !ba_transport_pthread_cleanup_lock(t);
		if (t->a2dp.pcm.fd == -1)
			return 0;
		ba_transport_pthread_cleanup_unlock(t);
		io->t_locked = false;
		goto repoll;
	case -1:
		if (errno == EINTR)
			goto repoll;
		return -1;
	}

	if (fds[0].revents & POLLIN) {
		/* dispatch incoming event */
		switch (ba_transport_recv_signal(t)) {
		case BA_TRANSPORT_SIGNAL_PCM_OPEN:
		case BA_TRANSPORT_SIGNAL_PCM_RESUME:
			io->asrs.frames = 0;
			io->timeout = -1;
			goto repoll;
		case BA_TRANSPORT_SIGNAL_PCM_CLOSE:
			/* reuse PCM read disconnection logic */
			break;
		case BA_TRANSPORT_SIGNAL_PCM_SYNC:
			io->timeout = 100;
			goto repoll;
		case BA_TRANSPORT_SIGNAL_PCM_DROP:
			io_thread_read_pcm_flush(&t->a2dp.pcm);
			goto repoll;
		default:
			goto repoll;
		}
	}

	ssize_t samples;
	switch (samples = io_thread_read_pcm(&t->a2dp.pcm, buffer->tail, ffb_len_in(buffer))) {
	case 0:
		io->timeout = config.a2dp.keep_alive * 1000;
		debug("Keep-alive polling: %d", io->timeout);
		goto repoll;
	case -1:
		if (errno == EAGAIN)
			goto repoll;
		return -1;
	}

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	/* When the thread is created, there might be no data in the FIFO. In fact
	 * there might be no data for a long time - until client starts playback.
	 * In order to correctly calculate time drift, the zero time point has to
	 * be obtained after the stream has started. */
	if (io->asrs.frames == 0)
		asrsync_init(&io->asrs, t->a2dp.pcm.sampling);

	/* scale volume or mute audio signal */
	io_thread_scale_pcm(&t->a2dp.pcm, buffer->tail, samples);

	/* update PCM buffer */
	ffb_seek(buffer, samples);

	/* return overall number of samples */
	return ffb_len_out(buffer);
}

/**
 * Validate BT socket for reading. */
static int a2dp_validate_bt_sink(struct ba_transport *t) {

	if (t->bt_fd == -1) {
		error("Invalid BT socket: %d", t->bt_fd);
		return -1;
	}

	/* Check for invalid (e.g. not set) reading MTU. If buffer allocation does
	 * not return NULL (allocating zero bytes might return NULL), we will read
	 * zero bytes from the BT socket, which will be wrongly identified as a
	 * "connection closed" action. */
	if (t->mtu_read <= 0) {
		error("Invalid reading MTU: %zu", t->mtu_read);
		return -1;
	}

	return 0;
}

/**
 * Poll and read BT signal from the SEQPACKET socket.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
static ssize_t a2dp_poll_and_read_bt(struct ba_transport *t,
		struct io_thread_data *io, ffb_uint8_t *buffer) {
	(void)io;

	struct pollfd fds[2] = {
		{ t->sig_fd[0], POLLIN, 0 },
		{ -1, POLLIN, 0 }};

	/* Allow escaping from the poll() by thread cancellation. */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

repoll:

	/* Add BT socket to the poll if transport is active. */
	fds[1].fd = t->state == BA_TRANSPORT_STATE_ACTIVE ? t->bt_fd : -1;

	if (poll(fds, ARRAYSIZE(fds), -1) == -1) {
		if (errno == EINTR)
			goto repoll;
		error("Transport poll error: %s", strerror(errno));
		return -1;
	}

	if (fds[0].revents & POLLIN) {
		/* dispatch incoming event */
		ba_transport_recv_signal(t);
		goto repoll;
	}

	ssize_t len;
	if ((len = read(fds[1].fd, buffer->tail, ffb_len_in(buffer))) == -1) {
		debug("BT read error: %s", strerror(errno));
		goto repoll;
	}

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	/* it seems that zero is never returned... */
	if (len == 0) {
		debug("BT socket has been closed: %d", fds[1].fd);
		/* Prevent sending the release request to the BlueZ. If the socket has
		 * been closed, it means that BlueZ has already closed the connection. */
		close(fds[1].fd);
		t->bt_fd = -1;
		return 0;
	}

	return len;
}

/**
 * Initialize RTP headers.
 *
 * @param s The memory area where the RTP headers will be initialized.
 * @param hdr The address where the pointer to the RTP header will be stored.
 * @param phdr The address where the pointer to the RTP payload header will
 *   be stored. This parameter might be NULL.
 * @param phdr_size The size of the RTP payload header.
 * @return This function returns the address of the RTP payload region. */
static uint8_t *a2dp_init_rtp(void *s, rtp_header_t **hdr,
		void **phdr, size_t phdr_size) {

	rtp_header_t *header = *hdr = (rtp_header_t *)s;
	memset(header, 0, RTP_HEADER_LEN + phdr_size);
	header->paytype = 96;
	header->version = 2;
	header->seq_number = random();
	header->timestamp = random();

	uint8_t *data = (uint8_t *)&header->csrc[header->cc];

	if (phdr != NULL)
		*phdr = data;

	return data + phdr_size;
}

static void *a2dp_sink_sbc(struct ba_transport *t) {

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		/* Lock transport during initialization stage. This lock will ensure,
		 * that no one will modify critical section until thread state can be
		 * known - initialization has failed or succeeded. */
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	if (a2dp_validate_bt_sink(t) != 0)
		goto fail_init;

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->a2dp.configuration, t->a2dp.codec->cfg_size)) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	ffb_uint8_t bt = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);

	if (ffb_init(&pcm, sbc_get_codesize(&sbc)) == NULL ||
			ffb_init(&bt, t->mtu_read) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	/* Lock transport during thread cancellation. This handler shall be at
	 * the top of the cleanup stack - lastly pushed. */
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	uint16_t seq_number = -1;
#if DEBUG
	uint16_t sbc_bitpool = 0;
#endif

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(t, &io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (t->a2dp.pcm.fd == -1) {
			seq_number = -1;
			continue;
		}

		const rtp_header_t *rtp_header = (rtp_header_t *)bt.data;
		const rtp_media_header_t *rtp_media_header = (rtp_media_header_t *)&rtp_header->csrc[rtp_header->cc];
		const uint8_t *rtp_payload = (uint8_t *)(rtp_media_header + 1);
		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)rtp_header);

#if ENABLE_PAYLOADCHECK
		if (rtp_header->paytype < 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}
#endif

		uint16_t _seq_number = be16toh(rtp_header->seq_number);
		if (++seq_number != _seq_number) {
			if (seq_number != 0)
				warn("Missing RTP packet: %u != %u", _seq_number, seq_number);
			seq_number = _seq_number;
		}

		/* decode retrieved SBC frames */
		size_t frames = rtp_media_header->frame_count;
		while (frames--) {

			ssize_t len;
			size_t decoded;

			if ((len = sbc_decode(&sbc, rtp_payload, rtp_payload_len,
							pcm.data, ffb_blen_in(&pcm), &decoded)) < 0) {
				error("SBC decoding error: %s", strerror(-len));
				break;
			}

#if DEBUG
			if (sbc_bitpool != sbc.bitpool) {
				sbc_bitpool = sbc.bitpool;
				sbc_print_internals(&sbc);
			}
#endif

			rtp_payload += len;
			rtp_payload_len -= len;

			const size_t samples = decoded / sizeof(int16_t);
			io_thread_scale_pcm(&t->a2dp.pcm, pcm.data, samples);
			if (io_thread_write_pcm(&t->a2dp.pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

		}

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

static void *a2dp_source_sbc(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		.timeout = -1,
		/* Lock transport during initialization stage. This lock will ensure,
		 * that no one will modify critical section until thread state can be
		 * known - initialization has failed or succeeded. */
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->a2dp.configuration, t->a2dp.codec->cfg_size)) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	ffb_uint8_t bt = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);

	const a2dp_sbc_t *configuration = (a2dp_sbc_t *)t->a2dp.configuration;
	const size_t sbc_pcm_samples = sbc_get_codesize(&sbc) / sizeof(int16_t);
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;

	/* initialize SBC encoder bit-pool */
	sbc.bitpool = sbc_a2dp_get_bitpool(configuration, config.sbc_quality);

#if DEBUG
	sbc_print_internals(&sbc);
#endif

	/* Writing MTU should be big enough to contain RTP header, SBC payload
	 * header and at least one SBC frame. In general, there is no constraint
	 * for the MTU value, but the speed might suffer significantly. */
	const size_t mtu_write_payload = t->mtu_write - RTP_HEADER_LEN - sizeof(rtp_media_header_t);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);

	if (mtu_write_payload < sbc_frame_len)
		warn("Writing MTU too small for one single SBC frame: %zu < %zu",
				t->mtu_write, RTP_HEADER_LEN + sizeof(rtp_media_header_t) + sbc_frame_len);

	if (ffb_init(&pcm, sbc_pcm_samples * (mtu_write_payload / sbc_frame_len)) == NULL ||
			ffb_init(&bt, t->mtu_write) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;

	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = a2dp_init_rtp(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(t, &io, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			goto fail;
		}

		/* anchor for RTP payload */
		bt.tail = rtp_payload;

		const int16_t *input = pcm.data;
		size_t input_len = samples;
		size_t output_len = ffb_len_in(&bt);
		size_t pcm_frames = 0;
		size_t sbc_frames = 0;

		/* Generate as many SBC frames as possible, but less than a 4-bit media
		 * header frame counter can contain. The size of the output buffer is
		 * based on the socket MTU, so such transfer should be most efficient. */
		while (input_len >= sbc_pcm_samples &&
				output_len >= sbc_frame_len &&
				sbc_frames < ((1 << 4) - 1)) {

			ssize_t len;
			ssize_t encoded;

			if ((len = sbc_encode(&sbc, input, input_len * sizeof(int16_t),
							bt.tail, output_len, &encoded)) < 0) {
				error("SBC encoding error: %s", strerror(-len));
				break;
			}

			len = len / sizeof(int16_t);
			input += len;
			input_len -= len;
			ffb_seek(&bt, encoded);
			output_len -= encoded;
			pcm_frames += len / channels;
			sbc_frames++;

		}

		rtp_header->seq_number = htobe16(++seq_number);
		rtp_header->timestamp = htobe32(timestamp);
		rtp_media_header->frame_count = sbc_frames;

		io.coutq.i = (io.coutq.i + 1) % ARRAYSIZE(io.coutq.v);
		if (io_thread_write_bt(t->bt_fd, bt.data, ffb_len_out(&bt),
					&io.coutq.v[io.coutq.i], t->a2dp.bt_fd_coutq_init) == -1) {
			if (errno == ECONNRESET || errno == ENOTCONN) {
				/* exit thread upon BT socket disconnection */
				debug("BT socket disconnected: %d", t->bt_fd);
				goto fail;
			}
			error("BT socket write error: %s", strerror(errno));
		}

		/* keep data transfer at a constant bit rate, also
		 * get a timestamp for the next RTP frame */
		asrsync_sync(&io.asrs, pcm_frames);
		timestamp += pcm_frames * 10000 / samplerate;

		/* update busy delay (encoding overhead) */
		t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.asrs) / 100;

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_len);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

#if ENABLE_MP3LAME || ENABLE_MPG123
static void *a2dp_sink_mpeg(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	if (a2dp_validate_bt_sink(t) != 0)
		goto fail_init;

#if ENABLE_MPG123

	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, (void (*)(void))mpg123_init);

	int err;
	mpg123_handle *handle;
	if ((handle = mpg123_new(NULL, &err)) == NULL) {
		error("Couldn't initialize MPG123 decoder: %s", mpg123_plain_strerror(err));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(mpg123_delete), handle);

	if (mpg123_open_feed(handle) != MPG123_OK) {
		error("Couldn't open MPG123 feed: %s", mpg123_strerror(handle));
		goto fail_open;
	}

	#define MPEG_PCM_DECODE_SAMPLES 4096

#else

	hip_t handle;
	if ((handle = hip_decode_init()) == NULL) {
		error("Couldn't initialize LAME decoder: %s", strerror(errno));
		goto fail_init;
	}

	const unsigned int channels = t->a2dp.pcm.channels;
	pthread_cleanup_push(PTHREAD_CLEANUP(hip_decode_exit), handle);

	/* NOTE: Size of the output buffer is "hard-coded" in hip_decode(). What is
	 *       even worse, the boundary check is so fucked-up that the hard-coded
	 *       limit can very easily overflow. In order to mitigate crash, we are
	 *       going to provide very big buffer - let's hope it will be enough. */
	#define MPEG_PCM_DECODE_SAMPLES 4096 * 100

#endif

	ffb_uint8_t bt = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);

	if (ffb_init(&pcm, MPEG_PCM_DECODE_SAMPLES) == NULL ||
			ffb_init(&bt, t->mtu_read) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	uint16_t seq_number = -1;

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(t, &io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (t->a2dp.pcm.fd == -1) {
			seq_number = -1;
			continue;
		}

		const rtp_header_t *rtp_header = (rtp_header_t *)bt.data;
		uint8_t *rtp_mpeg = (uint8_t *)&rtp_header->csrc[rtp_header->cc] + sizeof(rtp_mpeg_audio_header_t);
		size_t rtp_mpeg_len = len - (rtp_mpeg - (uint8_t *)rtp_header);

#if ENABLE_PAYLOADCHECK
		if (rtp_header->paytype < 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}
#endif

		uint16_t _seq_number = be16toh(rtp_header->seq_number);
		if (++seq_number != _seq_number) {
			if (seq_number != 0)
				warn("Missing RTP packet: %u != %u", _seq_number, seq_number);
			seq_number = _seq_number;
		}

#if ENABLE_MPG123

		long rate;
		int channels;
		int encoding;

decode:
		switch (mpg123_decode(handle, rtp_mpeg, rtp_mpeg_len,
					(uint8_t *)pcm.data, ffb_blen_in(&pcm), (size_t *)&len)) {
		case MPG123_DONE:
		case MPG123_NEED_MORE:
		case MPG123_OK:
			break;
		case MPG123_NEW_FORMAT:
			mpg123_getformat(handle, &rate, &channels, &encoding);
			debug("MPG123 new format detected: r:%ld, ch:%d, enc:%#x", rate, channels, encoding);
			break;
		default:
			error("MPG123 decoding error: %s", mpg123_strerror(handle));
			continue;
		}

		const size_t samples = len / sizeof(int16_t);
		io_thread_scale_pcm(&t->a2dp.pcm, pcm.data, samples);
		if (io_thread_write_pcm(&t->a2dp.pcm, pcm.data, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		if (len > 0) {
			rtp_mpeg_len = 0;
			goto decode;
		}

#else

		int16_t pcm_l[MPEG_PCM_DECODE_SAMPLES];
		int16_t pcm_r[MPEG_PCM_DECODE_SAMPLES];
		ssize_t samples;

		if ((samples = hip_decode(handle, rtp_mpeg, rtp_mpeg_len, pcm_l, pcm_r)) < 0) {
			error("LAME decoding error: %zd", samples);
			continue;
		}

		if (channels == 1) {
			io_thread_scale_pcm(&t->a2dp.pcm, pcm_l, samples);
			if (io_thread_write_pcm(&t->a2dp.pcm, pcm_l, samples) == -1)
				error("FIFO write error: %s", strerror(errno));
		}
		else {

			ssize_t i;
			for (i = 0; i < samples; i++) {
				pcm.data[i * 2 + 0] = pcm_l[i];
				pcm.data[i * 2 + 1] = pcm_r[i];
			}

			io_thread_scale_pcm(&t->a2dp.pcm, pcm.data, samples);
			if (io_thread_write_pcm(&t->a2dp.pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

		}

#endif

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
#if ENABLE_MPG123
fail_open:
#endif
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_MP3LAME
static void *a2dp_source_mp3(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		.timeout = -1,
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	lame_t handle;
	if ((handle = lame_init()) == NULL) {
		error("Couldn't initialize LAME encoder: %s", strerror(errno));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(lame_close), handle);

	const a2dp_mpeg_t *configuration = (a2dp_mpeg_t *)t->a2dp.configuration;
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
	MPEG_mode mode = NOT_SET;

	lame_set_num_channels(handle, channels);
	lame_set_in_samplerate(handle, samplerate);

	switch (configuration->channel_mode) {
	case MPEG_CHANNEL_MODE_MONO:
		mode = MONO;
		break;
	case MPEG_CHANNEL_MODE_DUAL_CHANNEL:
		mode = DUAL_CHANNEL;
		break;
	case MPEG_CHANNEL_MODE_STEREO:
		mode = STEREO;
		break;
	case MPEG_CHANNEL_MODE_JOINT_STEREO:
		mode = JOINT_STEREO;
		break;
	}

	if (lame_set_mode(handle, mode) != 0) {
		error("LAME: Couldn't set mode: %d", mode);
		goto fail_setup;
	}
	if (lame_set_bWriteVbrTag(handle, 0) != 0) {
		error("LAME: Couldn't disable VBR header");
		goto fail_setup;
	}
	if (lame_set_error_protection(handle, configuration->crc) != 0) {
		error("LAME: Couldn't set CRC mode: %d", configuration->crc);
		goto fail_setup;
	}
	if (configuration->vbr) {
		if (lame_set_VBR(handle, vbr_default) != 0) {
			error("LAME: Couldn't set VBR mode: %d", vbr_default);
			goto fail_setup;
		}
		if (lame_set_VBR_q(handle, config.lame_vbr_quality) != 0) {
			error("LAME: Couldn't set VBR quality: %d", config.lame_vbr_quality);
			goto fail_setup;
		}
	}
	else {
		if (lame_set_VBR(handle, vbr_off) != 0) {
			error("LAME: Couldn't set CBR mode");
			goto fail_setup;
		}
		int mpeg_bitrate = MPEG_GET_BITRATE(*configuration);
		int bitrate = a2dp_mpeg1_mp3_get_max_bitrate(mpeg_bitrate);
		if (lame_set_brate(handle, bitrate) != 0) {
			error("LAME: Couldn't set CBR bitrate: %d", bitrate);
			goto fail_setup;
		}
		if (mpeg_bitrate & MPEG_BIT_RATE_FREE &&
				lame_set_free_format(handle, 1) != 0) {
			error("LAME: Couldn't enable free format");
			goto fail_setup;
		}
	}
	if (lame_set_quality(handle, config.lame_quality) != 0) {
		error("LAME: Couldn't set quality: %d", config.lame_quality);
		goto fail_setup;
	}

	if (lame_init_params(handle) != 0) {
		error("LAME: Couldn't setup encoder");
		goto fail_setup;
	}

	ffb_uint8_t bt = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);

	const size_t mpeg_pcm_samples = lame_get_framesize(handle);
	/* It is hard to tell the size of the buffer required, but
	 * empirical test shows that 2KB should be sufficient. */
	const size_t mpeg_frame_len = 2048;

	if (ffb_init(&pcm, mpeg_pcm_samples) == NULL ||
			ffb_init(&bt, RTP_HEADER_LEN + sizeof(rtp_mpeg_audio_header_t) + mpeg_frame_len) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	rtp_header_t *rtp_header;
	rtp_mpeg_audio_header_t *rtp_mpeg_audio_header;

	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = a2dp_init_rtp(bt.data, &rtp_header,
			(void **)&rtp_mpeg_audio_header, sizeof(*rtp_mpeg_audio_header));
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(t, &io, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			goto fail;
		}

		/* anchor for RTP payload */
		bt.tail = rtp_payload;

		size_t pcm_frames = samples / channels;
		ssize_t len;

		if ((len = channels == 1 ?
					lame_encode_buffer(handle, pcm.data, NULL, pcm_frames, bt.tail, ffb_len_in(&bt)) :
					lame_encode_buffer_interleaved(handle, pcm.data, pcm_frames, bt.tail, ffb_len_in(&bt))) < 0) {
			error("LAME encoding error: %s", lame_encode_strerror(len));
			continue;
		}

		if (len > 0) {

			size_t payload_len_max = t->mtu_write - RTP_HEADER_LEN - sizeof(*rtp_mpeg_audio_header);
			size_t payload_len_total = len;
			size_t payload_len = len;
			rtp_header->timestamp = htobe32(timestamp);

			for (;;) {

				ssize_t ret;
				size_t len;

				len = payload_len > payload_len_max ? payload_len_max : payload_len;
				rtp_header->markbit = payload_len <= payload_len_max;
				rtp_header->seq_number = htobe16(++seq_number);
				rtp_mpeg_audio_header->offset = payload_len_total - payload_len;

				io.coutq.i = (io.coutq.i + 1) % ARRAYSIZE(io.coutq.v);
				if ((ret = io_thread_write_bt(t->bt_fd, bt.data, RTP_HEADER_LEN +
								sizeof(*rtp_mpeg_audio_header) + len, &io.coutq.v[io.coutq.i],
								t->a2dp.bt_fd_coutq_init)) == -1) {
					if (errno == ECONNRESET || errno == ENOTCONN) {
						/* exit thread upon BT socket disconnection */
						debug("BT socket disconnected: %d", t->bt_fd);
						goto fail;
					}
					error("BT socket write error: %s", strerror(errno));
					break;
				}

				/* account written payload only */
				ret -= RTP_HEADER_LEN + sizeof(*rtp_mpeg_audio_header);

				/* break if the last part of the payload has been written */
				if ((payload_len -= ret) == 0)
					break;

				/* move rest of data to the beginning of the payload */
				debug("Payload fragmentation: extra %zd bytes", payload_len);
				memmove(rtp_payload, rtp_payload + ret, payload_len);

			}

		}

		/* keep data transfer at a constant bit rate, also
		 * get a timestamp for the next RTP frame */
		asrsync_sync(&io.asrs, pcm_frames);
		timestamp += pcm_frames * 10000 / samplerate;

		/* update busy delay (encoding overhead) */
		t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.asrs) / 100;

		/* If the input buffer was not consumed (due to frame alignment), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, pcm_frames * channels);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_setup:
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_AAC
static void *a2dp_sink_aac(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	if (a2dp_validate_bt_sink(t) != 0)
		goto fail_open;

	HANDLE_AACDECODER handle;
	AAC_DECODER_ERROR err;

	if ((handle = aacDecoder_Open(TT_MP4_LATM_MCP1, 1)) == NULL) {
		error("Couldn't open AAC decoder");
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(aacDecoder_Close), handle);

	const unsigned int channels = t->a2dp.pcm.channels;
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

	ffb_uint8_t bt = { 0 };
	ffb_uint8_t latm = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &latm);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);

	if (ffb_init(&pcm, 2048 * channels) == NULL ||
			ffb_init(&latm, t->mtu_read) == NULL ||
			ffb_init(&bt, t->mtu_read) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	uint16_t seq_number = -1;
	int markbit_quirk = -3;

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(t, &io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (t->a2dp.pcm.fd == -1) {
			seq_number = -1;
			continue;
		}

		const rtp_header_t *rtp_header = (rtp_header_t *)bt.data;
		uint8_t *rtp_latm = (uint8_t *)&rtp_header->csrc[rtp_header->cc];
		size_t rtp_latm_len = len - (rtp_latm - (uint8_t *)rtp_header);

#if ENABLE_PAYLOADCHECK
		if (rtp_header->paytype < 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}
#endif

		/* If in the first N packets mark bit is not set, it might mean, that
		 * the mark bit will not be set at all. In such a case, activate mark
		 * bit quirk workaround. */
		if (markbit_quirk < 0) {
			if (rtp_header->markbit)
				markbit_quirk = 0;
			else if (++markbit_quirk == 0) {
				warn("Activating RTP mark bit quirk workaround");
				markbit_quirk = 1;
			}
		}

		uint16_t _seq_number = be16toh(rtp_header->seq_number);
		if (++seq_number != _seq_number) {
			if (seq_number != 0)
				warn("Missing RTP packet: %u != %u", _seq_number, seq_number);
			seq_number = _seq_number;
		}

		if (ffb_len_in(&latm) < rtp_latm_len) {
			debug("Resizing LATM buffer: %zd -> %zd", latm.size, latm.size + t->mtu_read);
			size_t prev_len = ffb_len_out(&latm);
			ffb_init(&latm, latm.size + t->mtu_read);
			ffb_seek(&latm, prev_len);
		}

		memcpy(latm.tail, rtp_latm, rtp_latm_len);
		ffb_seek(&latm, rtp_latm_len);

		if (markbit_quirk != 1 && !rtp_header->markbit) {
			debug("Fragmented RTP packet [%u]: LATM len: %zd", seq_number, rtp_latm_len);
			continue;
		}

		unsigned int data_len = ffb_len_out(&latm);
		unsigned int valid = ffb_len_out(&latm);
		CStreamInfo *aacinf;

		if ((err = aacDecoder_Fill(handle, &latm.data, &data_len, &valid)) != AAC_DEC_OK)
			error("AAC buffer fill error: %s", aacdec_strerror(err));
		else if ((err = aacDecoder_DecodeFrame(handle, pcm.tail, ffb_blen_in(&pcm), 0)) != AAC_DEC_OK)
			error("AAC decode frame error: %s", aacdec_strerror(err));
		else if ((aacinf = aacDecoder_GetStreamInfo(handle)) == NULL)
			error("Couldn't get AAC stream info");
		else {
			const size_t samples = aacinf->frameSize * aacinf->numChannels;
			io_thread_scale_pcm(&t->a2dp.pcm, pcm.data, samples);
			if (io_thread_write_pcm(&t->a2dp.pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));
		}

		/* make room for new LATM frame */
		ffb_rewind(&latm);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
fail_ffb:
	pthread_cleanup_pop(1);
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
static void *a2dp_source_aac(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		.timeout = -1,
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	HANDLE_AACENCODER handle;
	AACENC_InfoStruct aacinf;
	AACENC_ERROR err;

	const a2dp_aac_t *configuration = (a2dp_aac_t *)t->a2dp.configuration;
	const unsigned int bitrate = AAC_GET_BITRATE(*configuration);
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;

	/* create AAC encoder without the Meta Data module */
	if ((err = aacEncOpen(&handle, 0x07, channels)) != AACENC_OK) {
		error("Couldn't open AAC encoder: %s", aacenc_strerror(err));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(aacEncClose), &handle);

	unsigned int aot = AOT_NONE;
	unsigned int channelmode = channels == 1 ? MODE_1 : MODE_2;

	switch (configuration->object_type) {
	case AAC_OBJECT_TYPE_MPEG2_AAC_LC:
#if AACENCODER_LIB_VERSION <= 0x03040C00 /* 3.4.12 */ || \
		AACENCODER_LIB_VERSION >= 0x04000000 /* 4.0.0 */
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
	if (configuration->vbr) {
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
#if AACENCODER_LIB_VERSION >= 0x03041600 /* 3.4.22 */
	if ((err = aacEncoder_SetParam(handle, AACENC_AUDIOMUXVER, config.aac_latm_version)) != AACENC_OK) {
		error("Couldn't set LATM version: %s", aacenc_strerror(err));
		goto fail_init;
	}
#endif

	if ((err = aacEncEncode(handle, NULL, NULL, NULL, NULL)) != AACENC_OK) {
		error("Couldn't initialize AAC encoder: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncInfo(handle, &aacinf)) != AACENC_OK) {
		error("Couldn't get encoder info: %s", aacenc_strerror(err));
		goto fail_init;
	}

	ffb_uint8_t bt = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);

	if (ffb_init(&pcm, aacinf.inputChannels * aacinf.frameLength) == NULL ||
			ffb_init(&bt, RTP_HEADER_LEN + aacinf.maxOutBufBytes) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	rtp_header_t *rtp_header;

	/* initialize RTP header and get anchor for payload */
	uint8_t *rtp_payload = a2dp_init_rtp(bt.data, &rtp_header, NULL, 0);
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	int in_bufferIdentifiers[] = { IN_AUDIO_DATA };
	int out_bufferIdentifiers[] = { OUT_BITSTREAM_DATA };
	int in_bufSizes[] = { pcm.size * sizeof(*pcm.data) };
	int out_bufSizes[] = { aacinf.maxOutBufBytes };
	int in_bufElSizes[] = { sizeof(*pcm.data) };
	int out_bufElSizes[] = { sizeof(*bt.data) };

	AACENC_BufDesc in_buf = {
		.numBufs = 1,
		.bufs = (void **)&pcm.data,
		.bufferIdentifiers = in_bufferIdentifiers,
		.bufSizes = in_bufSizes,
		.bufElSizes = in_bufElSizes,
	};
	AACENC_BufDesc out_buf = {
		.numBufs = 1,
		.bufs = (void **)&rtp_payload,
		.bufferIdentifiers = out_bufferIdentifiers,
		.bufSizes = out_bufSizes,
		.bufElSizes = out_bufElSizes,
	};
	AACENC_InArgs in_args = { 0 };
	AACENC_OutArgs out_args = { 0 };

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(t, &io, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			goto fail;
		}

		while ((in_args.numInSamples = ffb_len_out(&pcm)) > 0) {

			if ((err = aacEncEncode(handle, &in_buf, &out_buf, &in_args, &out_args)) != AACENC_OK)
				error("AAC encoding error: %s", aacenc_strerror(err));

			if (out_args.numOutBytes > 0) {

				size_t payload_len_max = t->mtu_write - RTP_HEADER_LEN;
				size_t payload_len = out_args.numOutBytes;
				rtp_header->timestamp = htobe32(timestamp);

				/* If the size of the RTP packet exceeds writing MTU, the RTP payload
				 * should be fragmented. According to the RFC 3016, fragmentation of
				 * the audioMuxElement requires no extra header - the payload should
				 * be fragmented and spread across multiple RTP packets. */
				for (;;) {

					ssize_t ret;
					size_t len;

					len = payload_len > payload_len_max ? payload_len_max : payload_len;
					rtp_header->markbit = payload_len <= payload_len_max;
					rtp_header->seq_number = htobe16(++seq_number);

					io.coutq.i = (io.coutq.i + 1) % ARRAYSIZE(io.coutq.v);
					if ((ret = io_thread_write_bt(t->bt_fd, bt.data, RTP_HEADER_LEN + len,
									&io.coutq.v[io.coutq.i], t->a2dp.bt_fd_coutq_init)) == -1) {
						if (errno == ECONNRESET || errno == ENOTCONN) {
							/* exit thread upon BT socket disconnection */
							debug("BT socket disconnected: %d", t->bt_fd);
							goto fail;
						}
						error("BT socket write error: %s", strerror(errno));
						break;
					}

					/* account written payload only */
					ret -= RTP_HEADER_LEN;

					/* break if the last part of the payload has been written */
					if ((payload_len -= ret) == 0)
						break;

					/* move rest of data to the beginning of the payload */
					debug("Payload fragmentation: extra %zd bytes", payload_len);
					memmove(rtp_payload, rtp_payload + ret, payload_len);

				}

			}

			/* keep data transfer at a constant bit rate, also
			 * get a timestamp for the next RTP frame */
			unsigned int pcm_frames = out_args.numInSamples / channels;
			asrsync_sync(&io.asrs, pcm_frames);
			timestamp += pcm_frames * 10000 / samplerate;

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.asrs) / 100;

			/* If the input buffer was not consumed, we have to append new data to
			 * the existing one. Since we do not use ring buffer, we will simply
			 * move unprocessed data to the front of our linear buffer. */
			ffb_shift(&pcm, out_args.numInSamples);

		}

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
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

#if ENABLE_APTX
static void *a2dp_source_aptx(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		.timeout = -1,
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	APTXENC handle = malloc(SizeofAptxbtenc());
	pthread_cleanup_push(PTHREAD_CLEANUP(free), handle);

	if (handle == NULL || aptxbtenc_init(handle, __BYTE_ORDER == __LITTLE_ENDIAN) != 0) {
		error("Couldn't initialize apt-X encoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_uint8_t bt = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);

	const unsigned int channels = t->a2dp.pcm.channels;
	const size_t aptx_pcm_samples = 4 * channels;
	const size_t aptx_code_len = 2 * sizeof(uint16_t);
	const size_t mtu_write = t->mtu_write;

	if (ffb_init(&pcm, aptx_pcm_samples * (mtu_write / aptx_code_len)) == NULL ||
			ffb_init(&bt, mtu_write) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(t, &io, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			goto fail;
		}

		int16_t *input = pcm.data;
		size_t input_len = samples;

		/* encode and transfer obtained data */
		while (input_len >= aptx_pcm_samples) {

			size_t output_len = ffb_len_in(&bt);
			size_t pcm_frames = 0;

			/* Generate as many apt-X frames as possible to fill the output buffer
			 * without overflowing it. The size of the output buffer is based on
			 * the socket MTU, so such a transfer should be most efficient. */
			while (input_len >= aptx_pcm_samples && output_len >= aptx_code_len) {

				int32_t pcm_l[4] = { input[0], input[2], input[4], input[6] };
				int32_t pcm_r[4] = { input[1], input[3], input[5], input[7] };

				if (aptxbtenc_encodestereo(handle, pcm_l, pcm_r, (uint16_t *)bt.tail) != 0) {
					error("Apt-X encoding error: %s", strerror(errno));
					break;
				}

				input += 4 * channels;
				input_len -= 4 * channels;
				ffb_seek(&bt, aptx_code_len);
				output_len -= aptx_code_len;
				pcm_frames += 4;

			}

			io.coutq.i = (io.coutq.i + 1) % ARRAYSIZE(io.coutq.v);
			if (io_thread_write_bt(t->bt_fd, bt.data, ffb_len_out(&bt),
						&io.coutq.v[io.coutq.i], t->a2dp.bt_fd_coutq_init) == -1) {
				if (errno == ECONNRESET || errno == ENOTCONN) {
					/* exit thread upon BT socket disconnection */
					debug("BT socket disconnected: %d", t->bt_fd);
					goto fail;
				}
				error("BT socket write error: %s", strerror(errno));
			}

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, pcm_frames);

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.asrs) / 100;

			/* reinitialize output buffer */
			ffb_rewind(&bt);

		}

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_len);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_APTX_HD
static void *a2dp_source_aptx_hd(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		.timeout = -1,
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	APTXENC handle = malloc(SizeofAptxhdbtenc());
	pthread_cleanup_push(PTHREAD_CLEANUP(free), handle);

	if (handle == NULL || aptxhdbtenc_init(handle, false) != 0) {
		error("Couldn't initialize apt-X HD encoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_uint8_t bt = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);

	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
	const size_t aptx_pcm_samples = 4 * channels;
	const size_t aptx_code_len = 2 * 3 * sizeof(uint8_t);
	const size_t mtu_write = t->mtu_write;

	if (ffb_init(&pcm, aptx_pcm_samples * ((mtu_write - RTP_HEADER_LEN) / aptx_code_len)) == NULL ||
			ffb_init(&bt, mtu_write) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	rtp_header_t *rtp_header;

	/* initialize RTP header and get anchor for payload */
	uint8_t *rtp_payload = a2dp_init_rtp(bt.data, &rtp_header, NULL, 0);
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(t, &io, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			goto fail;
		}

		int16_t *input = pcm.data;
		size_t input_len = samples;

		/* encode and transfer obtained data */
		while (input_len >= aptx_pcm_samples) {

			/* anchor for RTP payload */
			bt.tail = rtp_payload;

			size_t output_len = ffb_len_in(&bt);
			size_t pcm_frames = 0;

			/* Generate as many apt-X frames as possible to fill the output buffer
			 * without overflowing it. The size of the output buffer is based on
			 * the socket MTU, so such a transfer should be most efficient. */
			while (input_len >= aptx_pcm_samples && output_len >= aptx_code_len) {

				int32_t pcm_l[4] = { input[0] << 8, input[2] << 8, input[4] << 8, input[6] << 8 };
				int32_t pcm_r[4] = { input[1] << 8, input[3] << 8, input[5] << 8, input[7] << 8 };
				uint32_t code[2];

				if (aptxhdbtenc_encodestereo(handle, pcm_l, pcm_r, code) != 0) {
					error("Apt-X HD encoding error: %s", strerror(errno));
					break;
				}

				bt.tail[0] = ((uint8_t *)code)[2];
				bt.tail[1] = ((uint8_t *)code)[1];
				bt.tail[2] = ((uint8_t *)code)[0];
				bt.tail[3] = ((uint8_t *)code)[6];
				bt.tail[4] = ((uint8_t *)code)[5];
				bt.tail[5] = ((uint8_t *)code)[4];

				input += 4 * channels;
				input_len -= 4 * channels;
				ffb_seek(&bt, aptx_code_len);
				output_len -= aptx_code_len;
				pcm_frames += 4;

			}

			io.coutq.i = (io.coutq.i + 1) % ARRAYSIZE(io.coutq.v);
			if (io_thread_write_bt(t->bt_fd, bt.data, ffb_len_out(&bt),
						&io.coutq.v[io.coutq.i], t->a2dp.bt_fd_coutq_init) == -1) {
				if (errno == ECONNRESET || errno == ENOTCONN) {
					/* exit thread upon BT socket disconnection */
					debug("BT socket disconnected: %d", t->bt_fd);
					goto fail;
				}
				error("BT socket write error: %s", strerror(errno));
			}

			/* keep data transfer at a constant bit rate */
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
		ffb_shift(&pcm, samples - input_len);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_LDAC
static void *a2dp_source_ldac(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	struct io_thread_data io = {
		.timeout = -1,
		.t_locked = !ba_transport_pthread_cleanup_lock(t),
	};

	HANDLE_LDAC_BT handle;
	HANDLE_LDAC_ABR handle_abr;

	if ((handle = ldacBT_get_handle()) == NULL) {
		error("Couldn't open LDAC encoder: %s", strerror(errno));
		goto fail_open_ldac;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldacBT_free_handle), handle);

	if ((handle_abr = ldac_ABR_get_handle()) == NULL) {
		error("Couldn't open LDAC ABR: %s", strerror(errno));
		goto fail_open_ldac_abr;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldac_ABR_free_handle), handle_abr);

	const a2dp_ldac_t *configuration = (a2dp_ldac_t *)t->a2dp.configuration;
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
	const size_t ldac_pcm_samples = LDACBT_ENC_LSU * channels;

	if (ldacBT_init_handle_encode(handle, t->mtu_write - RTP_HEADER_LEN - sizeof(rtp_media_header_t),
				config.ldac_eqmid, configuration->channel_mode, LDACBT_SMPL_FMT_S16, samplerate) == -1) {
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

	ffb_uint8_t bt = { 0 };
	ffb_int16_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &pcm);

	if (ffb_init(&pcm, ldac_pcm_samples) == NULL ||
			ffb_init(&bt, t->mtu_write) == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup_lock), t);

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;

	/* initialize RTP headers and get anchor for payload */
	bt.tail = a2dp_init_rtp(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);
	size_t ts_frames = 0;

	ba_transport_pthread_cleanup_unlock(t);
	io.t_locked = false;

	debug("Starting IO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(t, &io, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			goto fail;
		}

		int16_t *input = pcm.data;
		size_t input_len = samples;

		/* encode and transfer obtained data */
		while (input_len >= ldac_pcm_samples) {

			int len;
			int encoded;
			int frames;

			if (ldacBT_encode(handle, input, &len, bt.tail, &encoded, &frames) != 0) {
				error("LDAC encoding error: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
				break;
			}

			rtp_media_header->frame_count = frames;

			frames = len / sizeof(int16_t);
			input += frames;
			input_len -= frames;

			if (encoded &&
					io_thread_write_bt(t->bt_fd, bt.data, ffb_len_out(&bt) + encoded,
						&io.coutq.v[0], t->a2dp.bt_fd_coutq_init) == -1) {
				if (errno == ECONNRESET || errno == ENOTCONN) {
					/* exit thread upon BT socket disconnection */
					debug("BT socket disconnected: %d", t->bt_fd);
					goto fail;
				}
				error("BT socket write error: %s", strerror(errno));
			}

			if (config.ldac_abr)
				ldac_ABR_Proc(handle, handle_abr, io.coutq.v[0] / t->mtu_write, 1);

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, frames / channels);
			ts_frames += frames;

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.asrs) / 100;

			if (encoded) {
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
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(!io.t_locked);
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
#endif

/**
 * Dump incoming BT data to a file. */
static void *a2dp_sink_dump(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	ffb_uint8_t bt = { 0 };
	FILE *f = NULL;
	char fname[64];
	char *ptr;

	sprintf(fname, "/tmp/ba-%s.dump", ba_transport_type_to_string(t->type));
	for (ptr = fname; *ptr != '\0'; ptr++) {
		*ptr = tolower(*ptr);
		if (*ptr == ' ' || *ptr == '(' || *ptr == ')')
			*ptr = '-';
	}

	debug("Opening BT dump file: %s", fname);
	if ((f = fopen(fname, "wb")) == NULL) {
		error("Couldn't create dump file: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(fclose), f);

	if (ffb_init(&bt, t->mtu_read) == NULL) {
		error("Couldn't create data buffer: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	for (;;) {
		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(t, NULL, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}
		debug("BT read: %zd", len);
		fwrite(bt.data, 1, len, f);
	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}

int a2dp_thread_create(struct ba_transport *t) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			return ba_transport_pthread_create(t, a2dp_source_sbc, "ba-a2dp-sbc");
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
#if ENABLE_MP3LAME
			if (((a2dp_mpeg_t *)t->a2dp.configuration)->layer == MPEG_LAYER_MP3)
				return ba_transport_pthread_create(t, a2dp_source_mp3, "ba-a2dp-mp3");
#endif
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return ba_transport_pthread_create(t, a2dp_source_aac, "ba-a2dp-aac");
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return ba_transport_pthread_create(t, a2dp_source_aptx, "ba-a2dp-aptx");
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return ba_transport_pthread_create(t, a2dp_source_aptx_hd, "ba-a2dp-aptx-hd");
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return ba_transport_pthread_create(t, a2dp_source_ldac, "ba-a2dp-ldac");
#endif
		}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			return ba_transport_pthread_create(t, a2dp_sink_sbc, "ba-a2dp-sbc");
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
#if ENABLE_MPG123
			return ba_transport_pthread_create(t, a2dp_sink_mpeg, "ba-a2dp-mpeg");
#elif ENABLE_MP3LAME
			if (((a2dp_mpeg_t *)t->a2dp.configuration)->layer == MPEG_LAYER_MP3)
				return ba_transport_pthread_create(t, a2dp_sink_mpeg, "ba-a2dp-mp3");
#endif
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return ba_transport_pthread_create(t, a2dp_sink_aac, "ba-a2dp-aac");
#endif
		}

	warn("Codec not supported: %u", t->type.codec);
	return -1;
}
