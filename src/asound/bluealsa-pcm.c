/*
 * bluealsa-pcm.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"


struct bluealsa_pcm {
	snd_pcm_ioplug_t io;

	/* D-Bus connection context */
	struct ba_dbus_ctx dbus_ctx;

	/* requested BlueALSA PCM */
	struct ba_pcm ba_pcm;
	size_t ba_pcm_buffer_size;
	int ba_pcm_fd;
	int ba_pcm_ctrl_fd;

	/* event file descriptor */
	int event_fd;

	/* virtual hardware - ring buffer */
	snd_pcm_uframes_t io_ptr;
	pthread_t io_thread;
	bool io_started;

	/* communication and encoding/decoding delay */
	snd_pcm_sframes_t delay;
	/* user provided extra delay component */
	snd_pcm_sframes_t delay_ex;

	/* ALSA operates on frames, we on bytes */
	size_t frame_size;

	/* In order to see whether the PCM has reached under-run (or over-run), we
	 * have to know the exact position of the hardware and software pointers.
	 * To do so, we could use the HW pointer provided by the IO plug structure.
	 * This pointer is updated by the snd_pcm_hwsync() function, which is not
	 * thread-safe (total disaster is guaranteed). Since we can not call this
	 * function, we are going to use our own HW pointer, which can be updated
	 * safely in our IO thread. */
	snd_pcm_uframes_t io_hw_boundary;
	snd_pcm_uframes_t io_hw_ptr;

};

/**
 * Helper debug macro for internal usage. */
#define debug2(M, ...) \
	debug("%s: " M, pcm->ba_pcm.pcm_path, ## __VA_ARGS__)

/**
 * Helper function for closing PCM transport. */
static int close_transport(struct bluealsa_pcm *pcm) {
	int rv = 0;
	if (pcm->ba_pcm_fd != -1) {
		rv |= close(pcm->ba_pcm_fd);
		pcm->ba_pcm_fd = -1;
	}
	if (pcm->ba_pcm_ctrl_fd != -1) {
		rv |= close(pcm->ba_pcm_ctrl_fd);
		pcm->ba_pcm_ctrl_fd = -1;
	}
	return rv;
}

/**
 * Helper function for IO thread termination. */
static void io_thread_cleanup(struct bluealsa_pcm *pcm) {
	debug2("IO thread cleanup");
	pcm->io_started = false;
}

/**
 * IO thread, which facilitates ring buffer. */
static void *io_thread(snd_pcm_ioplug_t *io) {

	struct bluealsa_pcm *pcm = io->private_data;
	pthread_cleanup_push(PTHREAD_CLEANUP(io_thread_cleanup), pcm);

	sigset_t sigset;
	sigemptyset(&sigset);

	/* Block signal, which will be used for pause/resume actions. */
	sigaddset(&sigset, SIGIO);
	/* Block SIGPIPE, so we could receive EPIPE while writing to the pipe
	 * whose reading end has been closed. This will allow clean playback
	 * termination. */
	sigaddset(&sigset, SIGPIPE);

	if ((errno = pthread_sigmask(SIG_BLOCK, &sigset, NULL)) != 0) {
		SNDERR("Thread signal mask error: %s", strerror(errno));
		goto fail;
	}

	/* Block I/O until a full period of frames is available. */
	bool started = false;

	struct asrsync asrs;
	asrsync_init(&asrs, io->rate);

	debug2("Starting IO loop: %d", pcm->ba_pcm_fd);
	for (;;) {

		int tmp;
		switch (io->state) {
		case SND_PCM_STATE_RUNNING:
			/* Some playback applications may start the PCM before they have
			 * written the first full period of audio data. To match the
			 * behavior of the ALSA hw plug-in we do not report an underrun in
			 * this case. Instead we pause the IO thread for the estimated time
			 * it should take for a real-time application to write the balance
			 * of the period. */
			if (!started && io->stream == SND_PCM_STREAM_PLAYBACK) {
				if (io->period_size > io->appl_ptr) {
					struct timespec ts = {
						.tv_nsec = (io->period_size - io->appl_ptr) * 1000000000 / io->rate };
					debug2("IO thread started with insufficient frames - pausing for %ld ms", ts.tv_nsec / 1000000);
					sigtimedwait(&sigset, NULL, &ts);
					asrsync_init(&asrs, io->rate);
					continue;
				}
				started = true;
			}
			break;
		case SND_PCM_STATE_DRAINING:
			break;
		case SND_PCM_STATE_DISCONNECTED:
			goto fail;
		case SND_PCM_STATE_XRUN:
			started = false;
			/* fall-through */
		default:
			debug2("IO thread paused: %d", io->state);
			sigwait(&sigset, &tmp);
			asrsync_init(&asrs, io->rate);
			debug2("IO thread resumed: %d", io->state);
		}

		snd_pcm_uframes_t io_ptr = pcm->io_ptr;
		snd_pcm_uframes_t io_buffer_size = io->buffer_size;
		snd_pcm_uframes_t io_hw_ptr = pcm->io_hw_ptr;
		snd_pcm_uframes_t io_hw_boundary = pcm->io_hw_boundary;
		snd_pcm_uframes_t frames = io->period_size;
		const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
		char *buffer = (char *)areas->addr + (areas->first + areas->step * io_ptr) / 8;
		char *head = buffer;
		ssize_t ret = 0;
		size_t len;

		/* If the leftover in the buffer is less than a whole period sizes,
		 * adjust the number of frames which should be transfered. It has
		 * turned out, that the buffer might contain fractional number of
		 * periods - it could be an ALSA bug, though, it has to be handled. */
		if (io_buffer_size - io_ptr < frames)
			frames = io_buffer_size - io_ptr;

		/* IO operation size in bytes */
		len = frames * pcm->frame_size;

		io_ptr += frames;
		if (io_ptr >= io_buffer_size)
			io_ptr -= io_buffer_size;

		io_hw_ptr += frames;
		if (io_hw_ptr >= io_hw_boundary)
			io_hw_ptr -= io_hw_boundary;

		if (io->stream == SND_PCM_STREAM_CAPTURE) {

			/* Read the whole period "atomically". This will assure, that frames
			 * are not fragmented, so the pointer can be correctly updated. */
			while (len != 0 && (ret = read(pcm->ba_pcm_fd, head, len)) != 0) {
				if (ret == -1) {
					if (errno == EINTR)
						continue;
					SNDERR("PCM FIFO read error: %s", strerror(errno));
					goto fail;
				}
				head += ret;
				len -= ret;
			}

			if (ret == 0)
				goto fail;

		}
		else {

			/* check for under-run and act accordingly */
			if (io_hw_ptr > io->appl_ptr) {
				io->state = SND_PCM_STATE_XRUN;
				io_ptr = -1;
				goto sync;
			}

			/* Perform atomic write - see the explanation above. */
			do {
				if ((ret = write(pcm->ba_pcm_fd, head, len)) == -1) {
					if (errno == EINTR)
						continue;
					SNDERR("PCM FIFO write error: %s", strerror(errno));
					goto fail;
				}
				head += ret;
				len -= ret;
			} while (len != 0);

			/* synchronize playback time */
			asrsync_sync(&asrs, frames);
		}

sync:
		pcm->io_ptr = io_ptr;
		pcm->io_hw_ptr = io_hw_ptr;
		eventfd_write(pcm->event_fd, 1);
	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	close_transport(pcm);
	eventfd_write(pcm->event_fd, 0xDEAD0000);
	return NULL;
}

static int bluealsa_start(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Starting");

	/* If the IO thread is already started, skip thread creation. Otherwise,
	 * we might end up with a bunch of IO threads reading or writing to the
	 * same FIFO simultaneously. Instead, just send resume signal. */
	if (pcm->io_started) {
		io->state = SND_PCM_STATE_RUNNING;
		pthread_kill(pcm->io_thread, SIGIO);
		return 0;
	}

	/* initialize delay calculation */
	pcm->delay = 0;

	if (!bluealsa_dbus_pcm_ctrl_send_resume(pcm->ba_pcm_ctrl_fd, NULL)) {
		debug2("Couldn't start PCM: %s", strerror(errno));
		return -errno;
	}

	/* State has to be updated before the IO thread is created - if the state
	 * does not indicate "running", the IO thread will be suspended until the
	 * "resume" signal is delivered. This requirement is only (?) theoretical,
	 * anyhow forewarned is forearmed. */
	snd_pcm_state_t prev_state = io->state;
	io->state = SND_PCM_STATE_RUNNING;

	pcm->io_started = true;
	if ((errno = pthread_create(&pcm->io_thread, NULL,
					PTHREAD_ROUTINE(io_thread), io)) != 0) {
		debug2("Couldn't create IO thread: %s", strerror(errno));
		pcm->io_started = false;
		io->state = prev_state;
		return -errno;
	}

	pthread_setname_np(pcm->io_thread, "pcm-io");
	return 0;
}

static int bluealsa_stop(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Stopping");

	if (pcm->io_started) {
		pcm->io_started = false;
		pthread_cancel(pcm->io_thread);
		pthread_join(pcm->io_thread, NULL);
	}

	if (!bluealsa_dbus_pcm_ctrl_send_drop(pcm->ba_pcm_ctrl_fd, NULL))
		return -errno;

	/* Although the pcm stream is now stopped, it is still prepared and
	 * therefore an application that uses start_threshold may try to restart it
	 * by simply performing an I/O operation. If such an application now calls
	 * poll() it may be blocked forever unless we generate an event here. */
	eventfd_write(pcm->event_fd, 1);

	return 0;
}

static snd_pcm_sframes_t bluealsa_pointer(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	if (pcm->ba_pcm_fd == -1)
		return -ENODEV;
	return pcm->io_ptr;
}

static int bluealsa_close(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Closing");
	bluealsa_dbus_connection_ctx_free(&pcm->dbus_ctx);
	close(pcm->event_fd);
	free(pcm);
	return 0;
}

static int bluealsa_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	(void)params;
	debug2("Initializing HW");

	pcm->frame_size = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_open(&pcm->dbus_ctx, pcm->ba_pcm.pcm_path,
				io->stream == SND_PCM_STREAM_PLAYBACK ? BA_PCM_FLAG_SINK : BA_PCM_FLAG_SOURCE,
				&pcm->ba_pcm_fd, &pcm->ba_pcm_ctrl_fd, &err)) {
		debug2("Couldn't open PCM: %s", err.message);
		dbus_error_free(&err);
		return -EBUSY;
	}

	/* Indicate that our PCM is ready for writing, even though is is not 100%
	 * true - IO thread is not running yet. Some weird implementations might
	 * require PCM to be writable before the snd_pcm_start() call. */
	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		eventfd_write(pcm->event_fd, 1);

	if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK) {
		/* By default, the size of the pipe buffer is set to a too large value for
		 * our purpose. On modern Linux system it is 65536 bytes. Large buffer in
		 * the playback mode might contribute to an unnecessary audio delay. Since
		 * it is possible to modify the size of this buffer we will set is to some
		 * low value, but big enough to prevent audio tearing. Note, that the size
		 * will be rounded up to the page size (typically 4096 bytes). */
		pcm->ba_pcm_buffer_size = fcntl(pcm->ba_pcm_fd, F_SETPIPE_SZ, 2048);
		debug2("FIFO buffer size: %zd", pcm->ba_pcm_buffer_size);
	}

	debug2("Selected HW buffer: %zd periods x %zd bytes %c= %zd bytes",
			io->buffer_size / io->period_size, pcm->frame_size * io->period_size,
			io->period_size * (io->buffer_size / io->period_size) == io->buffer_size ? '=' : '<',
			io->buffer_size * pcm->frame_size);

	return 0;
}

static int bluealsa_hw_free(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Freeing HW");
	if (close_transport(pcm) == -1)
		return -errno;
	return 0;
}

static int bluealsa_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Initializing SW");
	snd_pcm_sw_params_get_boundary(params, &pcm->io_hw_boundary);
	return 0;
}

static int bluealsa_prepare(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* if PCM FIFO is not opened, report it right away */
	if (pcm->ba_pcm_fd == -1)
		return -ENODEV;

	/* initialize ring buffer */
	pcm->io_hw_ptr = 0;
	pcm->io_ptr = 0;

	/* Indicate that our PCM is ready for i/o, even though is is not 100%
	 * true - the IO thread is not running yet. Applications using
	 * snd_pcm_sw_params_set_start_threshold() require PCM to be usable
	 * as soon as it has been prepared. */
	eventfd_write(pcm->event_fd, 1);

	debug2("Prepared");
	return 0;
}

static int bluealsa_drain(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	if (!bluealsa_dbus_pcm_ctrl_send_drain(pcm->ba_pcm_ctrl_fd, NULL))
		return -errno;
	return 0;
}

static int bluealsa_pause(snd_pcm_ioplug_t *io, int enable) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (!bluealsa_dbus_pcm_ctrl_send(pcm->ba_pcm_ctrl_fd,
				enable ? "Pause" : "Resume", NULL))
		return -errno;

	if (enable == 0) {
		io->state = SND_PCM_STATE_RUNNING;
		pthread_kill(pcm->io_thread, SIGIO);
	}

	/* Even though PCM transport is paused, our IO thread is still running. If
	 * the implementer relies on the PCM file descriptor readiness, we have to
	 * bump our internal event trigger. Otherwise, client might stuck forever
	 * in the poll/select system call. */
	eventfd_write(pcm->event_fd, 1);

	return 0;
}

static void bluealsa_dump(snd_pcm_ioplug_t *io, snd_output_t *out) {
	struct bluealsa_pcm *pcm = io->private_data;
	snd_output_printf(out, "BlueALSA PCM: %s\n", pcm->ba_pcm.pcm_path);
	snd_output_printf(out, "BlueALSA BlueZ device: %s\n", pcm->ba_pcm.device_path);
	snd_output_printf(out, "BlueALSA Bluetooth codec: %#x\n", pcm->ba_pcm.codec);
}

static int bluealsa_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (pcm->ba_pcm_fd == -1)
		return -ENODEV;

	/* Exact calculation of the PCM delay is very hard, if not impossible. For
	 * the sake of simplicity we will make few assumptions and approximations.
	 * In general, the delay is proportional to the number of bytes queued in
	 * the FIFO buffer, the time required to encode data, Bluetooth transfer
	 * latency and the time required by the device to decode and play audio. */

	snd_pcm_sframes_t delay = 0;
	unsigned int size;

	/* bytes queued in the PCM ring buffer */
	delay += io->appl_ptr - io->hw_ptr;

	/* bytes queued in the FIFO buffer */
	if (ioctl(pcm->ba_pcm_fd, FIONREAD, &size) != -1)
		delay += size / pcm->frame_size;

	/* data transfer (communication) and encoding/decoding */
	pcm->delay = (io->rate / 100) * pcm->ba_pcm.delay / 100;

	*delayp = delay + pcm->delay + pcm->delay_ex;
	return 0;
}

static int bluealsa_poll_descriptors_count(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	nfds_t dbus_nfds = 0;
	bluealsa_dbus_connection_poll_fds(&pcm->dbus_ctx, NULL, &dbus_nfds);

	return 1 + dbus_nfds;
}

static int bluealsa_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd,
		unsigned int nfds) {
	struct bluealsa_pcm *pcm = io->private_data;

	nfds_t dbus_nfds = nfds - 1;
	if (!bluealsa_dbus_connection_poll_fds(&pcm->dbus_ctx, &pfd[1], &dbus_nfds))
		return -EINVAL;

	/* PCM plug-in relies on our internal event file descriptor. */
	pfd[0].fd = pcm->event_fd;
	pfd[0].events = POLLIN;

	return 1 + dbus_nfds;
}

static int bluealsa_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
		unsigned int nfds, unsigned short *revents) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (bluealsa_dbus_connection_poll_dispatch(&pcm->dbus_ctx, &pfd[1], nfds - 1))
		while (dbus_connection_dispatch(pcm->dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
			continue;

	if (pcm->ba_pcm_fd == -1)
		return -ENODEV;

	if (pfd[0].revents & POLLIN) {

		eventfd_t event;
		eventfd_read(pcm->event_fd, &event);

		if (event & 0xDEAD0000)
			goto fail;

		/* ALSA expects that the event will match stream direction, e.g.
		 * playback will not start if the event is for reading. */
		*revents = io->stream == SND_PCM_STREAM_CAPTURE ? POLLIN : POLLOUT;

		/* Include POLLERR if PCM is not prepared, running or draining.
		 * Also restore the event trigger as in this state the io thread is
		 * not active to do it */
		if (io->state != SND_PCM_STATE_PREPARED &&
			 io->state != SND_PCM_STATE_RUNNING &&
			 io->state != SND_PCM_STATE_DRAINING) {
			*revents |= POLLERR;
			eventfd_write(pcm->event_fd, 1);
		}

		/* a playback application may write less than start_threshold frames on
		 * its first write and then wait in poll() forever because the event_fd
		 * never gets written to again.
		 * To prevent this possibility, we bump the internal trigger. */
		else if (snd_pcm_stream(io->pcm) == SND_PCM_STREAM_PLAYBACK &&
			io->state == SND_PCM_STATE_PREPARED)
			eventfd_write(pcm->event_fd, 1);

		/* If the event was triggered prematurely, wait for another one. */
		else if (!snd_pcm_avail_update(io->pcm))
			*revents = 0;
	}
	else
		*revents = 0;

	return 0;

fail:
	*revents = POLLERR | POLLHUP;
	return -ENODEV;
}

static const snd_pcm_ioplug_callback_t bluealsa_callback = {
	.start = bluealsa_start,
	.stop = bluealsa_stop,
	.pointer = bluealsa_pointer,
	.close = bluealsa_close,
	.hw_params = bluealsa_hw_params,
	.hw_free = bluealsa_hw_free,
	.sw_params = bluealsa_sw_params,
	.prepare = bluealsa_prepare,
	.drain = bluealsa_drain,
	.pause = bluealsa_pause,
	.dump = bluealsa_dump,
	.delay = bluealsa_delay,
	.poll_descriptors_count = bluealsa_poll_descriptors_count,
	.poll_descriptors = bluealsa_poll_descriptors,
	.poll_revents = bluealsa_poll_revents,
};

static int str2bdaddr(const char *str, bdaddr_t *ba) {

	unsigned int x[6];
	if (sscanf(str, "%x:%x:%x:%x:%x:%x",
				&x[5], &x[4], &x[3], &x[2], &x[1], &x[0]) != 6)
		return -1;

	size_t i;
	for (i = 0; i < 6; i++)
		ba->b[i] = x[i];

	return 0;
}

static int str2profile(const char *str) {
	if (strcasecmp(str, "a2dp") == 0)
		return BA_PCM_FLAG_PROFILE_A2DP;
	else if (strcasecmp(str, "sco") == 0)
		return BA_PCM_FLAG_PROFILE_SCO;
	return 0;
}

static snd_pcm_format_t get_snd_pcm_format(uint16_t format) {
	switch (format) {
	case 0x0008:
		return SND_PCM_FORMAT_U8;
	case 0x8010:
		return SND_PCM_FORMAT_S16_LE;
	case 0x8018:
		return SND_PCM_FORMAT_S24_3LE;
	default:
		SNDERR("Unsupported PCM format: %#x", format);
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

static int bluealsa_set_hw_constraint(struct bluealsa_pcm *pcm) {
	snd_pcm_ioplug_t *io = &pcm->io;

	static const snd_pcm_access_t accesses[] = {
		SND_PCM_ACCESS_MMAP_INTERLEAVED,
		SND_PCM_ACCESS_RW_INTERLEAVED,
	};

	int err;

	debug2("Setting constraints");

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
					ARRAYSIZE(accesses), accesses)) < 0)
		return err;

	unsigned int formats[] = { get_snd_pcm_format(pcm->ba_pcm.format) };
	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
					ARRAYSIZE(formats), formats)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS,
					2, 1024)) < 0)
		return err;

	/* In order to prevent audio tearing and minimize CPU utilization, we're
	 * going to setup buffer size constraints. These limits are derived from
	 * the transport sampling rate and the number of channels, so the buffer
	 * "time" size will be constant. The minimal period size and buffer size
	 * are respectively 10 ms and 200 ms. Upper limits are not constraint. */
	unsigned int min_p = pcm->ba_pcm.sampling * 10 / 1000 * pcm->ba_pcm.channels * 2;
	unsigned int min_b = pcm->ba_pcm.sampling * 200 / 1000 * pcm->ba_pcm.channels * 2;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					min_p, 1024 * 16)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
					min_b, 1024 * 1024 * 16)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
					pcm->ba_pcm.channels, pcm->ba_pcm.channels)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
					pcm->ba_pcm.sampling, pcm->ba_pcm.sampling)) < 0)
		return err;

	return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(bluealsa) {
	(void)root;

	snd_config_iterator_t i, next;
	const char *service = BLUEALSA_SERVICE;
	const char *device = NULL;
	const char *profile = NULL;
	struct bluealsa_pcm *pcm;
	long delay = 0;
	int ret;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);

		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (strcmp(id, "comment") == 0 ||
				strcmp(id, "type") == 0 ||
				strcmp(id, "hint") == 0)
			continue;

		if (strcmp(id, "service") == 0) {
			if (snd_config_get_string(n, &service) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "device") == 0) {
			if (snd_config_get_string(n, &device) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "profile") == 0) {
			if (snd_config_get_string(n, &profile) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "delay") == 0) {
			if (snd_config_get_integer(n, &delay) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	bdaddr_t ba_addr;
	if (device == NULL || str2bdaddr(device, &ba_addr) != 0) {
		SNDERR("Invalid BT device address: %s", device);
		return -EINVAL;
	}

	int ba_profile = 0;
	if (profile == NULL || (ba_profile = str2profile(profile)) == 0) {
		SNDERR("Invalid BT profile [a2dp, sco]: %s", profile);
		return -EINVAL;
	}

	if ((pcm = calloc(1, sizeof(*pcm))) == NULL)
		return -ENOMEM;

	pcm->event_fd = -1;
	pcm->ba_pcm_fd = -1;
	pcm->ba_pcm_ctrl_fd = -1;
	pcm->delay_ex = delay;

	dbus_threads_init_default();

	DBusError err = DBUS_ERROR_INIT;
	if (bluealsa_dbus_connection_ctx_init(&pcm->dbus_ctx, service, &err) != TRUE) {
		SNDERR("Couldn't initialize D-Bus context: %s", err.message);
		ret = -ENOMEM;
		goto fail;
	}

	int flags = ba_profile | (
			stream == SND_PCM_STREAM_PLAYBACK ? BA_PCM_FLAG_SINK : BA_PCM_FLAG_SOURCE);
	if (!bluealsa_dbus_get_pcm(&pcm->dbus_ctx, &ba_addr, flags, &pcm->ba_pcm, &err)) {
		SNDERR("Couldn't get BlueALSA PCM: %s", err.message);
		ret = -ENODEV;
		goto fail;
	}

	if ((pcm->event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
		ret = -errno;
		goto fail;
	}

	pcm->io.version = SND_PCM_IOPLUG_VERSION;
	pcm->io.name = "BlueALSA";
	pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
	pcm->io.mmap_rw = 1;
	pcm->io.callback = &bluealsa_callback;
	pcm->io.private_data = pcm;

	if ((ret = snd_pcm_ioplug_create(&pcm->io, name, stream, mode)) < 0)
		goto fail;

	if ((ret = bluealsa_set_hw_constraint(pcm)) < 0) {
		snd_pcm_ioplug_delete(&pcm->io);
		return ret;
	}

	*pcmp = pcm->io.pcm;
	return 0;

fail:
	bluealsa_dbus_connection_ctx_free(&pcm->dbus_ctx);
	dbus_error_free(&err);
	if (pcm->event_fd != -1)
		close(pcm->event_fd);
	free(pcm);
	return ret;
}

SND_PCM_PLUGIN_SYMBOL(bluealsa)
