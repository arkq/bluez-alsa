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
#include <stdint.h>
#include <unistd.h>

#include <glib.h>

#include "audio.h"
#include "bluealsa.h"
#include "shared/defs.h"
#include "shared/log.h"

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
	ssize_t ret = 0;

	const int fd = pcm->fd;
	if (fd != -1) {
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
	ssize_t ret = 0;

	do {

		const int fd = pcm->fd;
		if (fd == -1)
			goto final;

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
				ret = 0;
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
