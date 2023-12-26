/*
 * BlueALSA - io.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
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
#include "bluealsa-config.h"
#include "shared/defs.h"
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
	const double pcm_volume_ch_scales[2] = {
		pcm->volume[0].scale,
		pcm->volume[1].scale,
	};

	pthread_mutex_unlock(&pcm->mutex);

	if (!pcm_soft_volume) {
		/* In case of hardware volume control we will perform mute operation,
		 * because hardware muting is an equivalent of gain=0 which with some
		 * headsets does not entirely silence audio. */
		switch (pcm->format) {
		case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
			audio_silence_s16_2le(buffer, samples / channels, channels,
					pcm_volume_ch_scales[0] == 0, pcm_volume_ch_scales[1] == 0);
			break;
		case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
		case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
			audio_silence_s32_4le(buffer, samples / channels, channels,
					pcm_volume_ch_scales[0] == 0, pcm_volume_ch_scales[1] == 0);
			break;
		default:
			g_assert_not_reached();
		}
		return;
	}

	switch (pcm->format) {
	case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
		audio_scale_s16_2le(buffer, samples / channels, channels,
				pcm_volume_ch_scales[0], pcm_volume_ch_scales[1]);
		break;
	case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
	case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
		audio_scale_s32_4le(buffer, samples / channels, channels,
				pcm_volume_ch_scales[0], pcm_volume_ch_scales[1]);
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

static enum ba_transport_pcm_signal io_poll_signal_filter_none(
		enum ba_transport_pcm_signal signal,
		void *userdata) {
	(void)userdata;
	return signal;
}

/**
 * Poll and read data from the BT transport socket.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
ssize_t io_poll_and_read_bt(
		struct io_poll *io,
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t count) {

	struct pollfd fds[2] = {
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

	if (fds[0].revents & POLLIN) {
		/* dispatch incoming event */
		io_poll_signal_filter *filter = io->signal.filter != NULL ?
			io->signal.filter : io_poll_signal_filter_none;
		switch (filter(ba_transport_pcm_signal_recv(pcm), io->signal.userdata)) {
		default:
			goto repoll;
		}
	}

	return io_bt_read(pcm, buffer, count);
}

/**
 * Poll and read data from the PCM FIFO.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
ssize_t io_poll_and_read_pcm(
		struct io_poll *io,
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples) {

	struct pollfd fds[2] = {
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

	/* Poll for reading with optional sync timeout. */
	switch (poll_rv) {
	case 0:
		pthread_mutex_lock(&pcm->mutex);
		pcm->synced = true;
		pthread_mutex_unlock(&pcm->mutex);
		pthread_cond_signal(&pcm->cond);
		io->timeout = -1;
		return 0;
	case -1:
		if (errno == EINTR)
			goto repoll;
		return -1;
	}

	if (fds[0].revents & POLLIN) {
		/* dispatch incoming event */
		io_poll_signal_filter *filter = io->signal.filter != NULL ?
			io->signal.filter : io_poll_signal_filter_none;
		switch (filter(ba_transport_pcm_signal_recv(pcm), io->signal.userdata)) {
		case BA_TRANSPORT_PCM_SIGNAL_OPEN:
		case BA_TRANSPORT_PCM_SIGNAL_RESUME:
			io->asrs.frames = 0;
			io->timeout = -1;
			goto repoll;
		case BA_TRANSPORT_PCM_SIGNAL_CLOSE:
			/* reuse PCM read disconnection logic */
			break;
		case BA_TRANSPORT_PCM_SIGNAL_SYNC:
			io->timeout = 100;
			goto repoll;
		case BA_TRANSPORT_PCM_SIGNAL_DROP:
			/* Notify caller that the PCM FIFO has been dropped. This will give
			 * the caller a chance to reinitialize its internal state. */
			errno = ESTALE;
			return -1;
		default:
			goto repoll;
		}
	}

	if (fds[1].revents == 0)
		return 0;

	ssize_t samples_read;
	if ((samples_read = io_pcm_read(pcm, buffer, samples)) == -1) {
		if (errno == EAGAIN)
			goto repoll;
		if (errno != EBADF)
			return -1;
		samples_read = 0;
	}

	if (samples_read == 0)
		return 0;

	/* When the thread is created, there might be no data in the FIFO. In fact
	 * there might be no data for a long time - until client starts playback.
	 * In order to correctly calculate time drift, the zero time point has to
	 * be obtained after the stream has started. */
	if (io->asrs.frames == 0)
		asrsync_init(&io->asrs, pcm->sampling);

	return samples_read;
}
