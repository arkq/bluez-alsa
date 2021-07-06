/*
 * BlueALSA - io.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
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
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "audio.h"
#include "bluealsa.h"
#include "shared/defs.h"
#include "shared/log.h"

/**
 * Read data from the BT transport (SCO or SEQPACKET) socket. */
ssize_t io_bt_read(
		struct ba_transport_thread *th,
		void *buffer,
		size_t count) {

	const int fd = th->bt_fd;
	ssize_t ret;

	if (fd == -1)
		return errno = EBADFD, -1;

	while ((ret = read(fd, buffer, count)) == -1 &&
			errno == EINTR)
		continue;
	if (ret == -1 && (
				errno == ECONNABORTED ||
				errno == ECONNRESET ||
				errno == ETIMEDOUT)) {
		error("BT socket disconnected: %s", strerror(errno));
		ret = 0;
	}

	if (ret == 0)
		ba_transport_thread_bt_release(th);

	return ret;
}

/**
 * Write data to the BT transport (SCO or SEQPACKET) socket.
 *
 * Note:
 * This function may temporally re-enable thread cancellation! */
ssize_t io_bt_write(
		struct ba_transport_thread *th,
		const void *buffer,
		size_t count) {

	ssize_t ret;
	int fd;

retry:

	if ((fd = th->bt_fd) == -1)
		return errno = EBADFD, -1;

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
		case ECONNABORTED:
		case ECONNRESET:
		case ENOTCONN:
		case ETIMEDOUT:
			error("BT socket disconnected: %s", strerror(errno));
			ret = 0;
		}

	if (ret == 0)
		ba_transport_thread_bt_release(th);

	return ret;
}

/**
 * Scale PCM signal according to the volume configuration. */
void io_pcm_scale(
		const struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples) {

	const unsigned int channels = pcm->channels;
	size_t frames = samples / channels;

	if (!pcm->soft_volume) {
		/* In case of hardware volume control we will perform mute operation,
		 * because hardware muting is an equivalent of gain=0 which with some
		 * headsets does not entirely silence audio. */
		switch (pcm->format) {
		case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
			audio_silence_s16_2le(buffer, channels, frames,
					pcm->volume[0].muted, pcm->volume[1].muted);
			break;
		case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
		case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
			audio_silence_s32_4le(buffer, channels, frames,
					pcm->volume[0].muted, pcm->volume[1].muted);
			break;
		default:
			g_assert_not_reached();
		}
		return;
	}

	double ch1_scale = 0;
	double ch2_scale = 0;

	/* scaling based on the decibel formula pow(10, dB / 20) */
	if (!pcm->volume[0].muted)
		ch1_scale = pow(10, (0.01 * pcm->volume[0].level) / 20);
	if (!pcm->volume[1].muted)
		ch2_scale = pow(10, (0.01 * pcm->volume[1].level) / 20);

	switch (pcm->format) {
	case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
		audio_scale_s16_2le(buffer, channels, frames, ch1_scale, ch2_scale);
		break;
	case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
	case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
		audio_scale_s32_4le(buffer, channels, frames, ch1_scale, ch2_scale);
		break;
	default:
		g_assert_not_reached();
	}

}

/**
 * Flush read buffer of the transport PCM FIFO. */
ssize_t io_pcm_flush(struct ba_transport_pcm *pcm) {
	ssize_t rv = splice(pcm->fd, NULL, config.null_fd, NULL, 1024 * 32, SPLICE_F_NONBLOCK);
	if (rv > 0)
		rv /= BA_TRANSPORT_PCM_FORMAT_BYTES(pcm->format);
	else if (rv == -1 && errno == EAGAIN)
		rv = 0;
	return rv;
}

/**
 * Read PCM signal from the transport PCM FIFO. */
ssize_t io_pcm_read(
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples) {

	pthread_mutex_lock(&pcm->mutex);

	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(pcm->format);
	const int fd = pcm->fd;
	ssize_t ret = -1;

	if (fd == -1)
		errno = EBADFD;
	else {
		while ((ret = read(fd, buffer, samples * sample_size)) == -1 &&
				errno == EINTR)
			continue;
		if (ret == 0) {
			debug("PCM has been closed: %d", fd);
			ba_transport_pcm_release(pcm);
		}
	}

	pthread_mutex_unlock(&pcm->mutex);

	if (ret <= 0)
		return ret;

	samples = ret / sample_size;
	io_pcm_scale(pcm, buffer, samples);
	return samples;
}

/**
 * Write PCM signal to the transport PCM FIFO.
 *
 * Note:
 * This function may temporally re-enable thread cancellation! */
ssize_t io_pcm_write(
		struct ba_transport_pcm *pcm,
		const void *buffer,
		size_t samples) {

	pthread_mutex_lock(&pcm->mutex);

	size_t len = samples * BA_TRANSPORT_PCM_FORMAT_BYTES(pcm->format);
	ssize_t ret;

	do {

		const int fd = pcm->fd;
		if (fd == -1) {
			errno = EBADFD;
			ret = -1;
			goto final;
		}

		if ((ret = write(fd, buffer, len)) == -1)
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
				/* In order to provide a way of escaping from the infinite poll()
				 * we have to temporally re-enable thread cancellation. */
				pthread_cleanup_push(PTHREAD_CLEANUP(pthread_mutex_unlock), &pcm->mutex);
				pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
				struct pollfd pfd = { fd, POLLOUT, 0 };
				poll(&pfd, 1, -1);
				pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
				pthread_cleanup_pop(0);
				continue;
			case EPIPE:
				/* This errno value will be received only, when the SIGPIPE
				 * signal is caught, blocked or ignored. */
				debug("PCM has been closed: %d", fd);
				ba_transport_pcm_release(pcm);
				ret = 0;
				/* fall-through */
			default:
				goto final;
			}

		buffer += ret;
		len -= ret;

	} while (len != 0);

	/* It is guaranteed, that this function will write data atomically. */
	ret = samples;

final:
	pthread_mutex_unlock(&pcm->mutex);
	return ret;
}

static enum ba_transport_thread_signal io_poll_signal_filter_none(
		enum ba_transport_thread_signal signal,
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
		struct ba_transport_thread *th,
		void *buffer,
		size_t count) {

	struct pollfd fds[2] = {
		{ th->pipe[0], POLLIN, 0 },
		{ th->bt_fd, POLLIN, 0 }};

	/* Allow escaping from the poll() by thread cancellation. */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

repoll:

	if (poll(fds, ARRAYSIZE(fds), io->timeout) == -1) {
		if (errno == EINTR)
			goto repoll;
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		return -1;
	}

	if (fds[0].revents & POLLIN) {
		/* dispatch incoming event */
		io_poll_signal_filter *filter = io->signal.filter != NULL ?
			io->signal.filter : io_poll_signal_filter_none;
		enum ba_transport_thread_signal signal;
		ba_transport_thread_signal_recv(th, &signal);
		switch (filter(signal, io->signal.userdata)) {
		default:
			goto repoll;
		}
	}

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	return io_bt_read(th, buffer, count);
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

	struct ba_transport_thread *th = pcm->th;
	struct pollfd fds[2] = {
		{ th->pipe[0], POLLIN, 0 },
		{ -1, POLLIN, 0 }};

repoll:

	/* Allow escaping from the poll() by thread cancellation. */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

	/* Add PCM socket to the poll if it is active. */
	fds[1].fd = ba_transport_pcm_is_active(pcm) ? pcm->fd : -1;

	/* Poll for reading with optional sync timeout. */
	switch (poll(fds, ARRAYSIZE(fds), io->timeout)) {
	case 0:
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		pthread_cond_signal(&pcm->synced);
		io->timeout = -1;
		return 0;
	case -1:
		if (errno == EINTR)
			goto repoll;
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		return -1;
	}

	if (fds[0].revents & POLLIN) {
		/* dispatch incoming event */
		io_poll_signal_filter *filter = io->signal.filter != NULL ?
			io->signal.filter : io_poll_signal_filter_none;
		enum ba_transport_thread_signal signal;
		ba_transport_thread_signal_recv(th, &signal);
		switch (filter(signal, io->signal.userdata)) {
		case BA_TRANSPORT_THREAD_SIGNAL_PCM_OPEN:
		case BA_TRANSPORT_THREAD_SIGNAL_PCM_RESUME:
			io->asrs.frames = 0;
			io->timeout = -1;
			goto repoll;
		case BA_TRANSPORT_THREAD_SIGNAL_PCM_CLOSE:
			/* reuse PCM read disconnection logic */
			break;
		case BA_TRANSPORT_THREAD_SIGNAL_PCM_SYNC:
			io->timeout = 100;
			goto repoll;
		case BA_TRANSPORT_THREAD_SIGNAL_PCM_DROP:
			io_pcm_flush(pcm);
			goto repoll;
		default:
			goto repoll;
		}
	}

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	ssize_t samples_read;
	if ((samples_read = io_pcm_read(pcm, buffer, samples)) == -1) {
		if (errno == EAGAIN)
			goto repoll;
		if (errno != EBADFD)
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
