/*
 * bluealsa-pcm.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
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

#define BA_PAUSE_STATE_RUNNING 0
#define BA_PAUSE_STATE_PAUSED  (1 << 0)
#define BA_PAUSE_STATE_PENDING (1 << 1)

struct bluealsa_pcm {
	snd_pcm_ioplug_t io;

	/* D-Bus connection context */
	struct ba_dbus_ctx dbus_ctx;
	/* time of last D-Bus dispatching */
	struct timespec dbus_dispatch_ts;

	/* IO thread and application thread sync */
	pthread_mutex_t mutex;

	/* requested BlueALSA PCM */
	struct ba_pcm ba_pcm;
	size_t ba_pcm_buffer_size;
	int ba_pcm_fd;
	int ba_pcm_ctrl_fd;

	/* event file descriptor */
	int event_fd;

	/* virtual hardware - ring buffer */
	char *io_hw_buffer;
	/* The IO thread is responsible for maintaining the hardware pointer
	 * (pcm->io_hw_ptr), the application is responsible for the application
	 * pointer (io->appl_ptr). These are both volatile as they are both
	 * written in one thread and read in the other. */
	volatile snd_pcm_sframes_t io_hw_ptr;
	snd_pcm_uframes_t io_hw_boundary;
	/* Permit the application to modify the frequency of poll() events. */
	volatile snd_pcm_uframes_t io_avail_min;
	pthread_t io_thread;
	bool io_started;

	/* ALSA operates on frames, we on bytes */
	size_t frame_size;

	struct timespec delay_ts;
	snd_pcm_uframes_t delay_hw_ptr;
	unsigned int delay_pcm_nread;
	/* In the capture mode, delay_running indicates that frames are being
	 * transfered to the FIFO by the server. In playback mode it indicates
	 * that the IO thread is transferring frames to the FIFO. */
	bool delay_running;

	/* delay accumulated just before pausing */
	snd_pcm_sframes_t delay_paused;
	/* maximum delay in FIFO */
	snd_pcm_sframes_t delay_fifo_size;
	/* user provided extra delay component */
	snd_pcm_sframes_t delay_ex;

	/* synchronize threads to begin/end pause */
	pthread_cond_t pause_cond;
	unsigned int pause_state;

};

/**
 * Helper debug macro for internal usage. */
#define debug2(M, ...) \
	debug("%s: " M, pcm->ba_pcm.pcm_path, ## __VA_ARGS__)

#if SND_LIB_VERSION < 0x010106
/**
 * Get the available frames.
 *
 * This function is available in alsa-lib since version 1.1.6. For older
 * alsa-lib versions we need to provide our own implementation. */
static snd_pcm_uframes_t snd_pcm_ioplug_hw_avail(const snd_pcm_ioplug_t * const io,
		const snd_pcm_uframes_t hw_ptr, const snd_pcm_uframes_t appl_ptr) {
	struct bluealsa_pcm *pcm = io->private_data;
	snd_pcm_sframes_t diff;
	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		diff = appl_ptr - hw_ptr;
	else
		diff = io->buffer_size - hw_ptr + appl_ptr;
	if (diff < 0)
		diff += pcm->io_hw_boundary;
	return diff <= io->buffer_size ? (snd_pcm_uframes_t) diff : 0;
}
#endif

/**
 * Helper function for closing PCM transport. */
static int close_transport(struct bluealsa_pcm *pcm) {
	int rv = 0;
	pthread_mutex_lock(&pcm->mutex);
	if (pcm->ba_pcm_fd != -1) {
		rv |= close(pcm->ba_pcm_fd);
		pcm->ba_pcm_fd = -1;
	}
	if (pcm->ba_pcm_ctrl_fd != -1) {
		rv |= close(pcm->ba_pcm_ctrl_fd);
		pcm->ba_pcm_ctrl_fd = -1;
	}
	pthread_mutex_unlock(&pcm->mutex);
	return rv;
}

/**
 * Helper function for IO thread termination. */
static void io_thread_cleanup(struct bluealsa_pcm *pcm) {
	debug2("IO thread cleanup");
	pcm->io_started = false;
}

/**
 * Helper function for IO thread delay calculation. */
static void io_thread_update_delay(struct bluealsa_pcm *pcm,
		snd_pcm_sframes_t hw_ptr) {

	struct timespec now;
	unsigned int nread = 0;

	gettimestamp(&now);
	ioctl(pcm->ba_pcm_fd, FIONREAD, &nread);

	pthread_mutex_lock(&pcm->mutex);

	/* stash current time and levels */
	pcm->delay_ts = now;
	pcm->delay_pcm_nread = nread;
	if (hw_ptr == -1) {
		pcm->delay_hw_ptr = 0;
		if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK)
			pcm->delay_running = false;
	}
	else {
		pcm->delay_hw_ptr = hw_ptr;
		if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK)
			pcm->delay_running = true;
	}

	pthread_mutex_unlock(&pcm->mutex);

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

	struct asrsync asrs;
	asrsync_init(&asrs, io->rate);

	/* We update pcm->io_hw_ptr (i.e. the value seen by ioplug) only when
	 * a period has been completed. We use a temporary copy during the
	 * transfer procedure. */
	snd_pcm_uframes_t io_hw_ptr = pcm->io_hw_ptr;

	debug2("Starting IO loop: %d", pcm->ba_pcm_fd);
	for (;;) {

		if (pcm->pause_state & BA_PAUSE_STATE_PENDING ||
				pcm->io_hw_ptr == -1) {
			debug2("Pausing IO thread");

			pthread_mutex_lock(&pcm->mutex);
			pcm->pause_state = BA_PAUSE_STATE_PAUSED;
			pthread_cond_signal(&pcm->pause_cond);
			pthread_mutex_unlock(&pcm->mutex);

			int tmp;
			sigwait(&sigset, &tmp);

			pthread_mutex_lock(&pcm->mutex);
			pcm->pause_state = BA_PAUSE_STATE_RUNNING;
			pthread_mutex_unlock(&pcm->mutex);

			debug2("IO thread resumed");

			if (pcm->io_hw_ptr == -1)
				continue;
			if (pcm->ba_pcm_fd == -1)
				goto fail;

			asrsync_init(&asrs, io->rate);
			io_hw_ptr = pcm->io_hw_ptr;
		}

		if (io->state == SND_PCM_STATE_DISCONNECTED)
			goto fail;

		/* There are 2 reasons why the number of available frames may be
		 * zero: XRUN or drained final samples; we set the HW pointer to
		 * -1 to indicate we have no work to do. */
		snd_pcm_uframes_t avail;
		if ((avail = snd_pcm_ioplug_hw_avail(io, io_hw_ptr, io->appl_ptr)) == 0) {
			io_thread_update_delay(pcm, 0);
			pcm->io_hw_ptr = io_hw_ptr = -1;
			eventfd_write(pcm->event_fd, 1);
			continue;
		}

		/* current offset of the head pointer in the IO buffer */
		snd_pcm_uframes_t offset = io_hw_ptr % io->buffer_size;

		/* Transfer at most 1 period of frames in each iteration ... */
		snd_pcm_uframes_t frames = io->period_size;
		/* ... but do not try to transfer more frames than are available in the
		 * ring buffer! */
		if (frames > avail)
			frames = avail;

		/* When used with the rate plugin the buffer might contain a fractional
		 * number of periods. So if the leftover in the buffer is less than a
		 * whole period size, adjust the number of frames which should be
		 * transfered.  */
		if (io->buffer_size - offset < frames)
			frames = io->buffer_size - offset;

		/* IO operation size in bytes */
		size_t len = frames * pcm->frame_size;
		char *head = pcm->io_hw_buffer + offset * pcm->frame_size;

		/* Increment the HW pointer (with boundary wrap). */
		io_hw_ptr += frames;
		if (io_hw_ptr >= pcm->io_hw_boundary)
			io_hw_ptr -= pcm->io_hw_boundary;

		ssize_t ret = 0;
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

			io_thread_update_delay(pcm, io_hw_ptr);

		}
		else {

			/* Perform atomic write - see the explanation above. */
			do {
				if ((ret = write(pcm->ba_pcm_fd, head, len)) == -1) {
					if (errno == EINTR)
						continue;
					if (errno != EPIPE)
						SNDERR("PCM FIFO write error: %s", strerror(errno));
					goto fail;
				}
				head += ret;
				len -= ret;
			} while (len != 0);

			io_thread_update_delay(pcm, io_hw_ptr);

			/* synchronize playback time */
			asrsync_sync(&asrs, frames);

		}

		/* Make the new HW pointer value visible to the ioplug. */
		pcm->io_hw_ptr = io_hw_ptr;

		/* Wake application thread if enough space/frames is available. */
		if (frames + io->buffer_size - avail >= pcm->io_avail_min)
			eventfd_write(pcm->event_fd, 1);
	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	close_transport(pcm);
	eventfd_write(pcm->event_fd, 0xDEAD0000);
	pthread_cond_signal(&pcm->pause_cond);
	return NULL;
}

static int bluealsa_start(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Starting");

	/* If the IO thread is already started, skip thread creation. Otherwise,
	 * we might end up with a bunch of IO threads reading or writing to the
	 * same FIFO simultaneously. Instead, just send resume signal. */
	if (pcm->io_started) {
		pthread_kill(pcm->io_thread, SIGIO);
		return 0;
	}

	if (!bluealsa_dbus_pcm_ctrl_send_resume(pcm->ba_pcm_ctrl_fd, NULL)) {
		debug2("Couldn't start PCM: %s", strerror(errno));
		return -errno;
	}

	/* Initialize delay calculation - capture reception begins immediately,
	 * playback transmission begins only when first period has been written
	 * by the application. */
	pcm->delay_running = io->stream == SND_PCM_STREAM_CAPTURE ? true : false;
	gettimestamp(&pcm->delay_ts);

	/* start the IO thread */
	pcm->io_started = true;
	if ((errno = pthread_create(&pcm->io_thread, NULL,
					PTHREAD_ROUTINE(io_thread), io)) != 0) {
		debug2("Couldn't create IO thread: %s", strerror(errno));
		pcm->io_started = false;
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

	pcm->delay_running = false;
	pcm->delay_pcm_nread = 0;

	/* Bug in ioplug - if pcm->io_hw_ptr == -1 then it reports state
	 * SND_PCM_STATE_XRUN instead of SND_PCM_STATE_SETUP after PCM
	 * was stopped. */
	pcm->io_hw_ptr = 0;

	if (!bluealsa_dbus_pcm_ctrl_send_drop(pcm->ba_pcm_ctrl_fd, NULL))
		return -errno;

	/* Applications that call poll() after snd_pcm_drain() will be blocked
	 * forever unless we generate a poll() event here. */
	eventfd_write(pcm->event_fd, 1);

	return 0;
}

static snd_pcm_sframes_t bluealsa_pointer(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	if (pcm->ba_pcm_fd == -1) {
		/* The ioplug sets the PCM state to SND_PCM_STATE_XRUN
		 * when this function returns error, so setting the state
		 * here has no effect - but we do it anyway in the hope
		 * that one day ioplug will acknowledge that PCM devices
		 * can disconnect. So typically when an application attempts
		 * I/O on a disconnected bluealsa PCM it gets EPIPE with
		 * state SND_PCM_STATE_XRUN, and only later when it attempts
		 * to recover with snd_pcm_prepare() does it get ENODEV
		 * with state SND_PCM_STATE_DISCONNECTED. */
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);
		return -ENODEV;
	}
#ifndef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
	if (pcm->io_hw_ptr != -1)
		return pcm->io_hw_ptr % io->buffer_size;
#endif
	return pcm->io_hw_ptr;
}

static int bluealsa_close(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Closing");
	bluealsa_dbus_connection_ctx_free(&pcm->dbus_ctx);
	close(pcm->event_fd);
	pthread_mutex_destroy(&pcm->mutex);
	pthread_cond_destroy(&pcm->pause_cond);
	free(pcm);
	return 0;
}

static int bluealsa_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	(void)params;
	debug2("Initializing HW");

	pcm->frame_size = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_open_pcm(&pcm->dbus_ctx, pcm->ba_pcm.pcm_path,
				&pcm->ba_pcm_fd, &pcm->ba_pcm_ctrl_fd, &err)) {
		debug2("Couldn't open PCM: %s", err.message);
		dbus_error_free(&err);
		return -EBUSY;
	}

	if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK)
		/* By default, the size of the pipe buffer is set to a too large value for
		 * our purpose. On modern Linux system it is 65536 bytes. Large buffer in
		 * the playback mode might contribute to an unnecessary audio delay. Since
		 * it is possible to modify the size of this buffer we will set is to some
		 * low value, but big enough to prevent audio tearing. Note, that the size
		 * will be rounded up to the page size (typically 4096 bytes). */
		pcm->delay_fifo_size = fcntl(pcm->ba_pcm_fd, F_SETPIPE_SZ, 2048) / pcm->frame_size;
	else
		pcm->delay_fifo_size = fcntl(pcm->ba_pcm_fd, F_GETPIPE_SZ)  / pcm->frame_size;

	debug2("FIFO buffer size: %zd frames", pcm->delay_fifo_size);

	/* ALSA default for avail min is one period. */
	pcm->io_avail_min = io->period_size;

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

	snd_pcm_uframes_t avail_min;
	snd_pcm_sw_params_get_avail_min(params, &avail_min);
	if (avail_min != pcm->io_avail_min) {
		debug2("Changing SW avail min: %zu -> %zu", pcm->io_avail_min, avail_min);
		pcm->io_avail_min = avail_min;
	}

	return 0;
}

static int bluealsa_prepare(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* if PCM FIFO is not opened, report it right away */
	if (pcm->ba_pcm_fd == -1) {
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);
		return -ENODEV;
	}

	/* initialize ring buffer */
	pcm->io_hw_ptr = 0;

	/* The ioplug allocates and configures its channel area buffer when the
	 * HW parameters are fixed, but after calling bluealsa_hw_params(). So,
	 * this is the earliest opportunity for us to safely cache the ring
	 * buffer start address. */
	const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
	pcm->io_hw_buffer = (char *)areas->addr + areas->first / 8;

	/* Indicate that our PCM is ready for IO, even though is is not 100%
	 * true - the IO thread may not be running yet. Applications using
	 * snd_pcm_sw_params_set_start_threshold() require the PCM to be usable
	 * as soon as it has been prepared. */
	eventfd_write(pcm->event_fd, 1);

	debug2("Prepared");
	return 0;
}

static int bluealsa_drain(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	bluealsa_dbus_pcm_ctrl_send_drain(pcm->ba_pcm_ctrl_fd, NULL);
	/* We cannot recover from an error here. By returning zero we ensure that
	 * ioplug stops the pcm. Returning an error code would be interpreted by
	 * ioplug as an incomplete drain and would it leave the pcm running. */
	return 0;
}

/**
 * Calculate overall PCM delay.
 *
 * Exact calculation of the PCM delay is very hard, if not impossible. For
 * the sake of simplicity we will make few assumptions and approximations.
 * In general, the delay is proportional to the number of bytes queued in
 * the FIFO buffer, the time required to encode data, Bluetooth transfer
 * latency and the time required by the device to decode and play audio. */
static snd_pcm_sframes_t bluealsa_calculate_delay(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	snd_pcm_sframes_t delay = 0;

	/* if PCM is not started there should be no capture delay */
	if (!pcm->delay_running && io->stream == SND_PCM_STREAM_CAPTURE)
		return 0;

	struct timespec now;
	gettimestamp(&now);

	/* In most cases, dispatching D-Bus messages/signals should be done in the
	 * poll_revents() callback. However, this mode of operation requires client
	 * code to use ALSA polling API. If for some reasons, client simply writes
	 * samples to opened PCM and in the same time wants to know the delay, we
	 * have to process D-Bus messages here. Otherwise, the BlueALSA component
	 * of the delay - pcm->ba_pcm.delay - might not be up to date.
	 *
	 * This synchronous dispatching will be performed only if the last D-Bus
	 * dispatching was done more than one second ago - this should prioritize
	 * asynchronous dispatching in the poll_revents() callback. */
	if (pcm->dbus_dispatch_ts.tv_sec + 1 < now.tv_sec) {
		bluealsa_dbus_connection_dispatch(&pcm->dbus_ctx);
		gettimestamp(&pcm->dbus_dispatch_ts);
	}

	pthread_mutex_lock(&pcm->mutex);

	struct timespec diff;
	difftimespec(&now, &pcm->delay_ts, &diff);

	/* the maximum number of frames that can have been
	 * produced/consumed by the server since pcm->delay_ts */
	unsigned int tframes =
		(diff.tv_sec * 1000 + diff.tv_nsec / 1000000) * io->rate / 1000;

	/* the number of frames that were in the FIFO at pcm->delay_ts */
	snd_pcm_uframes_t fifo_delay = pcm->delay_pcm_nread / pcm->frame_size;

	if (io->stream == SND_PCM_STREAM_CAPTURE) {

		/* Start with maximum frames available in FIFO since pcm->delay_ts. */
		delay = fifo_delay + tframes;

		/* Adjust by the change in frames in the buffer. */
		if (io->state != SND_PCM_STATE_XRUN)
			delay += io->buffer_size - snd_pcm_ioplug_hw_avail(io, pcm->delay_hw_ptr, io->appl_ptr);

		/* impose upper limit */
		snd_pcm_sframes_t limit = pcm->delay_fifo_size + io->buffer_size;
		if (delay > limit)
			delay = limit;

	}
	else {

		delay = fifo_delay;

		/* The buffer_delay is the number of frames that were in the buffer at
		 * pcm->delay_ts, adjusted the number written by the application since
		 * then. */
		snd_pcm_sframes_t buffer_delay = 0;
		if (io->state != SND_PCM_STATE_XRUN)
			buffer_delay = snd_pcm_ioplug_hw_avail(io, pcm->delay_hw_ptr, io->appl_ptr);

		/* If the PCM is running, then some frames from the buffer may have been
		 * consumed, so we add them before adjusting for time elapsed. */
		if (pcm->delay_running)
			delay += buffer_delay;

		/* Adjust the total delay by the number of frames consumed. */
		if ((delay -= tframes) < 0)
			delay = 0;

		/* If the PCM is not running, then the frames in the buffer will not have
		 * been consumed since pcm->delay_ts, so we add them after the time
		 * elapsed adjustment. */
		if (!pcm->delay_running)
			delay += buffer_delay;
	}

	pthread_mutex_unlock(&pcm->mutex);

	/* data transfer (communication) and encoding/decoding */
	delay += (io->rate / 100) * pcm->ba_pcm.delay / 100;

	delay += pcm->delay_ex;

	return delay;
}

static int bluealsa_pause(snd_pcm_ioplug_t *io, int enable) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (enable == 1) {
		/* Synchronize the IO thread with an application thread to ensure that
		 * the server will not be paused while we are processing a transfer. */
		pthread_mutex_lock(&pcm->mutex);
		pcm->pause_state |= BA_PAUSE_STATE_PENDING;
		while (!(pcm->pause_state & BA_PAUSE_STATE_PAUSED) && pcm->ba_pcm_fd != -1)
			pthread_cond_wait(&pcm->pause_cond, &pcm->mutex);
		pthread_mutex_unlock(&pcm->mutex);
	}

	if (!bluealsa_dbus_pcm_ctrl_send(pcm->ba_pcm_ctrl_fd,
				enable ? "Pause" : "Resume", NULL))
		return -errno;

	if (enable == 0)
		pthread_kill(pcm->io_thread, SIGIO);
	else
		/* store current delay value */
		pcm->delay_paused = bluealsa_calculate_delay(io);

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
	snd_output_printf(out, "BlueALSA Bluetooth codec: %s\n", pcm->ba_pcm.codec);
	/* alsa-lib commits the PCM setup only if bluealsa_hw_params() returned
	 * success, so we only dump the ALSA PCM parameters if the BlueALSA PCM
	 * connection is established. */
	if (pcm->ba_pcm_fd >= 0) {
		snd_output_printf(out, "Its setup is:\n");
		snd_pcm_dump_setup(io->pcm, out);
	}
}

static int bluealsa_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (pcm->ba_pcm_fd == -1) {
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);
		return -ENODEV;
	}

	int ret = 0;
	*delayp = 0;

	switch (io->state) {
		case SND_PCM_STATE_PREPARED:
		case SND_PCM_STATE_RUNNING:
			*delayp = bluealsa_calculate_delay(io);
			break;
		case SND_PCM_STATE_PAUSED:
			*delayp = pcm->delay_paused;
			break;
		case SND_PCM_STATE_XRUN:
			*delayp = bluealsa_calculate_delay(io);
			ret = -EPIPE;
			break;
		case SND_PCM_STATE_SUSPENDED:
			ret = -ESTRPIPE;
			break;
		default:
			break;
	}

	return ret;
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

	*revents = 0;
	int ret = 0;

	bluealsa_dbus_connection_poll_dispatch(&pcm->dbus_ctx, &pfd[1], nfds - 1);
	while (dbus_connection_dispatch(pcm->dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
		continue;
	gettimestamp(&pcm->dbus_dispatch_ts);

	if (pcm->ba_pcm_fd == -1)
		goto fail;

	if (pfd[0].revents & POLLIN) {

		eventfd_t event;
		eventfd_read(pcm->event_fd, &event);

		if (event & 0xDEAD0000)
			goto fail;

		/* This call synchronizes the ring buffer pointers and updates the
		 * ioplug state. */
		snd_pcm_sframes_t avail = snd_pcm_avail(io->pcm);

		/* ALSA expects that the event will match stream direction, e.g.
		 * playback will not start if the event is for reading. */
		*revents = io->stream == SND_PCM_STREAM_CAPTURE ? POLLIN : POLLOUT;

		/* We hold the event fd ready, unless insufficient frames are
		 * available in the ring buffer. */
		bool ready = true;

		switch (io->state) {
			case SND_PCM_STATE_SETUP:
				ready = false;
				*revents = 0;
				break;
			case SND_PCM_STATE_PREPARED:
				/* capture poll should block forever */
				if (io->stream == SND_PCM_STREAM_CAPTURE) {
					ready = false;
					*revents = 0;
				}
				break;
			case SND_PCM_STATE_RUNNING:
				if ((snd_pcm_uframes_t)avail < pcm->io_avail_min) {
					ready = false;
					*revents = 0;
				}
				break;
			case SND_PCM_STATE_XRUN:
			case SND_PCM_STATE_PAUSED:
			case SND_PCM_STATE_SUSPENDED:
				*revents |= POLLERR;
				break;
			case SND_PCM_STATE_OPEN:
				*revents = POLLERR;
				ret = -EBADF;
				break;
			case SND_PCM_STATE_DISCONNECTED:
				goto fail;
			default:
				break;
		};

		if (ready)
			eventfd_write(pcm->event_fd, 1);

	}

	return ret;

fail:
	snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);
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
		return BA_PCM_TRANSPORT_A2DP_SOURCE | BA_PCM_TRANSPORT_A2DP_SINK;
	else if (strcasecmp(str, "sco") == 0)
		return BA_PCM_TRANSPORT_HFP_AG | BA_PCM_TRANSPORT_HFP_HF |
			BA_PCM_TRANSPORT_HSP_AG | BA_PCM_TRANSPORT_HSP_HS;
	return 0;
}

static snd_pcm_format_t get_snd_pcm_format(uint16_t format) {
	switch (format) {
	case 0x0108:
		return SND_PCM_FORMAT_U8;
	case 0x8210:
		return SND_PCM_FORMAT_S16_LE;
	case 0x8318:
		return SND_PCM_FORMAT_S24_3LE;
	case 0x8418:
		return SND_PCM_FORMAT_S24_LE;
	case 0x8420:
		return SND_PCM_FORMAT_S32_LE;
	default:
		SNDERR("Unknown PCM format: %#x", format);
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

static DBusHandlerResult bluealsa_dbus_msg_filter(DBusConnection *conn,
		DBusMessage *message, void *data) {
	struct bluealsa_pcm *pcm = (struct bluealsa_pcm *)data;
	(void)conn;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(message, &iter))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(dbus_message_get_path(message), pcm->ba_pcm.pcm_path) != 0 ||
			strcmp(dbus_message_get_interface(message), DBUS_INTERFACE_PROPERTIES) != 0 ||
			strcmp(dbus_message_get_member(message), "PropertiesChanged") != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *updated_interface;
	dbus_message_iter_get_basic(&iter, &updated_interface);
	dbus_message_iter_next(&iter);

	if (strcmp(updated_interface, BLUEALSA_INTERFACE_PCM) == 0)
		bluealsa_dbus_message_iter_get_pcm_props(&iter, NULL, &pcm->ba_pcm);

	return DBUS_HANDLER_RESULT_HANDLED;
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
	 * going to setup period size constraint. The limit is derived from the
	 * transport sampling rate and the number of channels, so the period
	 * "time" size will be constant, and should be about 10ms. The upper
	 * limit will not be constrained. */
	unsigned int min_p = pcm->ba_pcm.sampling / 100 * pcm->ba_pcm.channels *
		snd_pcm_format_physical_width(get_snd_pcm_format(pcm->ba_pcm.format)) / 8;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					min_p, 1024 * 1024)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
					2 * min_p, 2 * 1024 * 1024)) < 0)
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
	pthread_mutex_init(&pcm->mutex, NULL);
	pthread_cond_init(&pcm->pause_cond, NULL);
	pcm->pause_state = BA_PAUSE_STATE_RUNNING;

	dbus_threads_init_default();

	DBusError err = DBUS_ERROR_INIT;
	if (bluealsa_dbus_connection_ctx_init(&pcm->dbus_ctx, service, &err) != TRUE) {
		SNDERR("Couldn't initialize D-Bus context: %s", err.message);
		ret = -ENOMEM;
		goto fail;
	}

	if (!dbus_connection_add_filter(pcm->dbus_ctx.conn, bluealsa_dbus_msg_filter, pcm, NULL)) {
		SNDERR("Couldn't add D-Bus filter: %s", strerror(ENOMEM));
		ret = -ENOMEM;
		goto fail;
	}

	debug("Getting BlueALSA PCM: %s %s %s", snd_pcm_stream_name(stream), device, profile);
	if (!bluealsa_dbus_get_pcm(&pcm->dbus_ctx, &ba_addr, ba_profile,
				stream == SND_PCM_STREAM_PLAYBACK ? BA_PCM_MODE_SINK : BA_PCM_MODE_SOURCE,
				&pcm->ba_pcm, &err)) {
		SNDERR("Couldn't get BlueALSA PCM: %s", err.message);
		ret = -ENODEV;
		goto fail;
	}

	/* Subscribe for properties-changed signals but for the opened PCM only. */
	bluealsa_dbus_connection_signal_match_add(&pcm->dbus_ctx, pcm->dbus_ctx.ba_service,
			pcm->ba_pcm.pcm_path, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged",
			"arg0='"BLUEALSA_INTERFACE_PCM"'");

	if ((pcm->event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
		ret = -errno;
		goto fail;
	}

	pcm->io.version = SND_PCM_IOPLUG_VERSION;
	pcm->io.name = "BlueALSA";
	pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
#ifdef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
	pcm->io.flags |= SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;
#endif
	pcm->io.mmap_rw = 1;
	pcm->io.callback = &bluealsa_callback;
	pcm->io.private_data = pcm;

#if SND_LIB_VERSION >= 0x010102 && SND_LIB_VERSION <= 0x010103
	/* ALSA library thread-safe API functionality does not play well with ALSA
	 * IO-plug plug-ins. It causes deadlocks which often make our PCM plug-in
	 * unusable. As a workaround we are going to disable this functionality. */
	if (setenv("LIBASOUND_THREAD_SAFE", "0", 0) == -1)
		SNDERR("Couldn't disable ALSA thread-safe API: %s", strerror(errno));
#endif

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
