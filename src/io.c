/*
 * BlueALSA - io.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
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
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "audio.h"
#include "ba-config.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"

/**
 * Read data from the BT transport (SCO or SEQPACKET) socket. */
ssize_t io_bt_read(
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t count) {

	const int fd = pcm->fd_bt;
	ssize_t ret;

retry:
	if ((ret = read(fd, buffer, count)) == -1)
		switch (errno) {
		case EINTR:
			goto retry;
		case ECONNRESET:
		case ENOTCONN:
			debug("BT socket disconnected: %s", strerror(errno));
			ret = 0;
			break;
		case ECONNABORTED:
		case ETIMEDOUT:
			error("BT read error: %s", strerror(errno));
			ret = 0;
	}

	if (ret == 0)
		ba_transport_pcm_bt_release(pcm);

	return ret;
}

/**
 * Write data to the BT transport (SCO or SEQPACKET) socket.
 *
 * Note:
 * This function may temporally re-enable thread cancellation! */
ssize_t io_bt_write(
		struct ba_transport_pcm *pcm,
		const void *buffer,
		size_t count) {

	const int fd = pcm->fd_bt;
	ssize_t ret;

retry:
	if ((ret = write(fd, buffer, count)) == -1)
		switch (errno) {
		case EINTR:
			goto retry;
		case EAGAIN:
			/* In order to provide a way of escaping from the infinite poll()
			 * we have to temporally re-enable thread cancellation. */
			pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
			struct pollfd pfd = { fd, POLLOUT, 0 };
			poll(&pfd, 1, -1);
			pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
			goto retry;
		case ECONNRESET:
		case ENOTCONN:
			debug("BT socket disconnected: %s", strerror(errno));
			ret = 0;
			break;
		case ECONNABORTED:
		case ETIMEDOUT:
			error("BT write error: %s", strerror(errno));
			ret = 0;
		}

	if (ret == 0)
		ba_transport_pcm_bt_release(pcm);

	return ret;
}

/**
 * Scale PCM signal according to the volume configuration. */
void io_pcm_scale(
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples) {

	pthread_mutex_lock(&pcm->mutex);

	const unsigned int channels = pcm->channels;
	const bool pcm_soft_volume = pcm->soft_volume;

	double pcm_volume_ch_scales[ARRAYSIZE(pcm->volume)];
	for (size_t i = 0; i < ARRAYSIZE(pcm_volume_ch_scales); i++)
		pcm_volume_ch_scales[i] = pcm->volume[i].scale;

	pthread_mutex_unlock(&pcm->mutex);

	if (!pcm_soft_volume)
		/* In case of hardware volume control we will perform mute operation,
		 * because hardware muting is an equivalent of gain=0 which with some
		 * headsets does not entirely silence audio. */
		for (size_t i = 0; i < channels; i++)
			if (pcm_volume_ch_scales[i] != 0.0)
				pcm_volume_ch_scales[i] = 1.0;

	bool identity = true;
	/* Check whether volume scaling is required. */
	for (size_t i = 0; i < channels; i++)
		if (pcm_volume_ch_scales[i] != 1.0) {
			identity = false;
			break;
		}

	if (identity)
		/* Nothing to do - volume is set to 100%. */
		return;

	switch (pcm->format) {
	case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
		audio_scale_s16_2le(buffer, pcm_volume_ch_scales, channels, samples / channels);
		break;
	case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
	case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
		audio_scale_s32_4le(buffer, pcm_volume_ch_scales, channels, samples / channels);
		break;
	default:
		g_assert_not_reached();
	}

}

/**
 * Flush read buffer of the transport PCM FIFO. */
ssize_t io_pcm_flush(struct ba_transport_pcm *pcm) {

	ssize_t samples = 0;
	ssize_t rv;

	pthread_mutex_lock(&pcm->mutex);

	const int fd = pcm->fd;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(pcm->format);

	while ((rv = splice(fd, NULL, config.null_fd, NULL, 32 * 1024, SPLICE_F_NONBLOCK)) > 0) {
		debug("Flushed PCM samples [%d]: %zd", fd, rv / sample_size);
		samples += rv / sample_size;
	}

	pthread_mutex_unlock(&pcm->mutex);

	if (rv == -1 && errno != EAGAIN)
		return rv;

	return samples;
}

/**
 * Read PCM signal from the transport PCM FIFO. */
ssize_t io_pcm_read(
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples) {

	pthread_mutex_lock(&pcm->mutex);

	const int fd = pcm->fd;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(pcm->format);
	ssize_t ret;

	while ((ret = read(fd, buffer, samples * sample_size)) == -1 &&
			errno == EINTR)
		continue;

	if (ret == 0) {
		debug("PCM client closed connection: %d", fd);
		ba_transport_pcm_release(pcm);
	}

	pthread_mutex_unlock(&pcm->mutex);

	if (ret <= 0)
		return ret;

	samples = ret / sample_size;
	io_pcm_scale(pcm, buffer, samples);
	return samples;
}

/**
 * Write PCM signal to the transport PCM FIFO. */
ssize_t io_pcm_write(
		struct ba_transport_pcm *pcm,
		const void *buffer,
		size_t samples) {

	pthread_mutex_lock(&pcm->mutex);

	const int fd = pcm->fd;
	const uint8_t *buffer_ = buffer;
	size_t len = samples * BA_TRANSPORT_PCM_FORMAT_BYTES(pcm->format);
	ssize_t ret;

	do {

		if ((ret = write(fd, buffer_, len)) == -1)
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
				/* If the client is so slow that the FIFO fills up, then it
				 * is inevitable that audio frames will be eventually be
				 * dropped in the bluetooth controller if we block here.
				 * It is better that we discard frames here so that the
				 * decoder is not interrupted. */
				warn("Dropping PCM frames: %s", "PCM overrun");
				ret = len;
				break;
			case EPIPE:
				/* This errno value will be received only, when the SIGPIPE
				 * signal is caught, blocked or ignored. */
				debug("PCM client closed connection: %d", fd);
				ba_transport_pcm_release(pcm);
				ret = 0;
				/* fall-through */
			default:
				goto final;
			}

		buffer_ += ret;
		len -= ret;

	} while (len != 0);

	/* It is guaranteed, that this function will write data atomically. */
	ret = samples;

final:
	pthread_mutex_unlock(&pcm->mutex);
	return ret;
}

static void io_poll_drain_complete(
		struct io_poll *io,
		struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);
	pcm->drained = true;
	pthread_mutex_unlock(&pcm->mutex);
	pthread_cond_signal(&pcm->cond);

	io->tainted = false;
	io->draining = false;
	io->timeout = -1;

}

/**
 * Poll and read data from the BT transport socket.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
ssize_t io_poll_and_read_bt(
		struct io_poll *io,
		struct ba_transport_pcm *pcm,
		ffb_t *buffer) {

	struct pollfd fds[] = {
		{ pcm->pipe[0], POLLIN, 0 },
		{ pcm->fd_bt, POLLIN, 0 }};

repoll:

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	int poll_rv = poll(fds, ARRAYSIZE(fds), io->timeout);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	if (poll_rv == -1) {
		if (errno == EINTR)
			goto repoll;
		return -1;
	}

	if (fds[0].revents & POLLIN)
		switch (ba_transport_pcm_signal_recv(pcm)) {
		default:
			goto repoll;
		}

	ssize_t len;
	if ((len = io_bt_read(pcm, buffer->tail, ffb_blen_in(buffer))) > 0)
		ffb_seek(buffer, len);
	return len;
}

/**
 * Poll and read data from the PCM FIFO.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
ssize_t io_poll_and_read_pcm(
		struct io_poll *io,
		struct ba_transport_pcm *pcm,
		ffb_t *buffer) {

	struct pollfd fds[] = {
		{ pcm->pipe[0], POLLIN, 0 },
		{ -1, POLLIN, 0 }};

repoll:

	pthread_mutex_lock(&pcm->mutex);
	/* Add PCM socket to the poll if it is not paused. */
	fds[1].fd = pcm->paused ? -1 : pcm->fd;
	pthread_mutex_unlock(&pcm->mutex);

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	int poll_rv = poll(fds, ARRAYSIZE(fds), io->timeout);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	/* Poll for reading with optional drain timeout. */
	switch (poll_rv) {
	case 0:
		if (io->draining)
			/* If draining, use the logic in the read code block. */
			break;
		io_poll_drain_complete(io, pcm);
		return 0;
	case -1:
		if (errno == EINTR)
			goto repoll;
		if (io->draining)
			io_poll_drain_complete(io, pcm);
		return -1;
	}

	if (fds[0].revents & POLLIN)
		switch (ba_transport_pcm_signal_recv(pcm)) {
		case BA_TRANSPORT_PCM_SIGNAL_OPEN:
		case BA_TRANSPORT_PCM_SIGNAL_RESUME:
			io->asrs.frames = 0;
			io->initiated = false;
			io->draining = false;
			io->timeout = -1;
			goto repoll;
		case BA_TRANSPORT_PCM_SIGNAL_CLOSE:
			/* Reuse PCM read disconnection logic. */
			break;
		case BA_TRANSPORT_PCM_SIGNAL_DRAIN:
			io->draining = io->tainted;
			io->timeout = io->tainted ? 100 : 0;
			goto repoll;
		case BA_TRANSPORT_PCM_SIGNAL_DROP:
			if (io->draining)
				io_poll_drain_complete(io, pcm);
			/* Flush the PCM FIFO and drop all PCM data in the buffer. */
			io_pcm_flush(pcm);
			ffb_rewind(buffer);
			/* Notify caller that the PCM data has been dropped. This will give
			 * the caller a chance to reinitialize its internal state. */
			errno = ESTALE;
			return -1;
		default:
			goto repoll;
		}

	ssize_t samples;
	if ((samples = io_pcm_read(pcm, buffer->tail, ffb_len_in(buffer))) == -1) {
		switch (errno) {
		case EAGAIN:
			if (!io->draining)
				goto repoll;
			/* The FIFO is now empty, but we must still ensure that any
			 * remaining frames in the encoder buffer are flushed to BT.
			 * We pad the buffer with silence to ensure the encoder
			 * codesize minimum frames are available. */
			samples = ffb_len_in(buffer);
			memset(buffer->tail, 0, ffb_blen_in(buffer));
			debug("Padding PCM with silence [%d]: %zd", fds[1].fd, samples);
			/* Make sure that the next time this function is called we
			 * flag that the drain is complete. */
			io->draining = false;
			io->timeout = 0;
			break;
		case EBADF:
			samples = 0;
			break;
		}
	}

	if (samples <= 0) {
		if (io->draining)
			io_poll_drain_complete(io, pcm);
		return samples;
	}

	/* When the thread is created, there might be no data in the FIFO. In fact
	 * there might be no data for a long time - until client starts playback.
	 * In order to correctly calculate time drift, the zero time point has to
	 * be obtained after the stream has started. */
	if (io->asrs.frames == 0)
		asrsync_init(&io->asrs, pcm->rate);

	/* Mark the IO as tainted, so in case of a drain operation we will
	 * flush any remaining frames in the encoder buffers to BT. */
	io->tainted = true;

	ffb_seek(buffer, samples);
	return samples;
}
