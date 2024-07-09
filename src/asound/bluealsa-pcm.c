/*
 * bluealsa-pcm.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/hex.h"
#include "shared/log.h"
#include "shared/rt.h"

#define BA_PAUSE_STATE_RUNNING 0
#define BA_PAUSE_STATE_PAUSED  (1 << 0)
#define BA_PAUSE_STATE_PENDING (1 << 1)

#if SND_LIB_VERSION >= 0x010104 && SND_LIB_VERSION < 0x010206
#include <alloca.h>
/**
 * alsa-lib releases from 1.1.4 to 1.2.5.1 inclusive have a bug in the rate
 * plugin which, when combined with the hw params refinement algorithm used by
 * the ioplug, can cause snd_pcm_avail() to return bogus values. This, in turn,
 * can trigger deadlock in applications built on the portaudio library
 * (e.g. audacity) and possibly cause faults in other applications too.
 *
 * This macro enables a work-around for this bug.
 * */
# define BLUEALSA_HW_PARAMS_FIX 1
#endif

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

	/* PCM FIFO */
	int ba_pcm_fd;
	/* PCM control socket */
	int ba_pcm_ctrl_fd;

	/* Indicates that the server is connected. */
	atomic_bool connected;

	/* event file descriptor */
	int event_fd;

	/* virtual hardware - ring buffer */
	char * _Atomic io_hw_buffer;
	/* The IO thread is responsible for maintaining the hardware pointer
	 * (pcm->io_hw_ptr), the application is responsible for the application
	 * pointer (io->appl_ptr). These pointers should be atomic as they are
	 * written in one thread and read in the other. */
	_Atomic snd_pcm_sframes_t io_hw_ptr;
	_Atomic snd_pcm_uframes_t io_hw_boundary;
	/* Permit the application to modify the frequency of poll() events. */
	_Atomic snd_pcm_uframes_t io_avail_min;
	pthread_t io_thread;
	bool io_started;

	/* ALSA operates on frames, we on bytes */
	size_t frame_size;

	struct timespec delay_ts;
	snd_pcm_uframes_t delay_hw_ptr;
	unsigned int delay_pcm_nread;
	/* In the capture mode, delay_running indicates that frames are being
	 * transferred to the FIFO by the server. In playback mode it indicates
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
	snd_pcm_uframes_t diff_ = diff;
	return diff_ <= io->buffer_size ? diff_ : 0;
}
#endif

/**
 * Helper function for terminating IO thread. */
static void io_thread_cancel(struct bluealsa_pcm *pcm) {

	if (!pcm->io_started)
		return;

	pthread_cancel(pcm->io_thread);
	pthread_join(pcm->io_thread, NULL);
	pcm->io_started = false;

}

/**
 * Helper function for logging IO thread termination. */
static void io_thread_cleanup(struct bluealsa_pcm *pcm) {
	debug2("IO thread cleanup");
	(void)pcm;
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
	/* Block all signals in the IO thread.
	 * Especially, we need to block SIGPIPE, so we could receive EPIPE while
	 * writing to the pipe which reading end was closed by the server. This
	 * will allow clean playback termination. Also, we need to block SIGIO,
	 * which is used for pause/resume actions. The rest of the signals are
	 * blocked because we are using thread cancellation and we do not want
	 * any interference from signal handlers. */
	sigfillset(&sigset);
	if ((errno = pthread_sigmask(SIG_SETMASK, &sigset, NULL)) != 0) {
		SNDERR("Thread signal mask error: %s", strerror(errno));
		goto fail;
	}

	struct asrsync asrs;
	asrsync_init(&asrs, io->rate);

	/* We update pcm->io_hw_ptr (i.e. the value seen by ioplug) only when
	 * a period has been completed. We use a temporary copy during the
	 * transfer procedure. */
	snd_pcm_sframes_t io_hw_ptr = pcm->io_hw_ptr;

	debug2("Starting IO loop: %d", pcm->ba_pcm_fd);
	for (;;) {

		pthread_mutex_lock(&pcm->mutex);
		unsigned int is_pause_pending = pcm->pause_state & BA_PAUSE_STATE_PENDING;
		pthread_mutex_unlock(&pcm->mutex);

		if (is_pause_pending ||
				pcm->io_hw_ptr == -1) {
			debug2("Pausing IO thread");

			pthread_mutex_lock(&pcm->mutex);
			pcm->pause_state = BA_PAUSE_STATE_PAUSED;
			pthread_mutex_unlock(&pcm->mutex);
			pthread_cond_signal(&pcm->pause_cond);

			int tmp;
			sigwait(&sigset, &tmp);

			pthread_mutex_lock(&pcm->mutex);
			pcm->pause_state = BA_PAUSE_STATE_RUNNING;
			pthread_mutex_unlock(&pcm->mutex);

			debug2("IO thread resumed");

			if (pcm->io_hw_ptr == -1)
				continue;

			asrsync_init(&asrs, io->rate);
			io_hw_ptr = pcm->io_hw_ptr;
		}

		/* There are 2 reasons why the number of available frames may be
		 * zero: XRUN or drained final samples; we set the HW pointer to
		 * -1 to indicate we have no work to do. */
		snd_pcm_uframes_t avail;
		if ((avail = snd_pcm_ioplug_hw_avail(io, io_hw_ptr, io->appl_ptr)) == 0) {
			pcm->io_hw_ptr = io_hw_ptr = -1;
			io_thread_update_delay(pcm, io_hw_ptr);
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

		/* Increment the HW pointer (with boundary wrap). */
		io_hw_ptr += frames;
		if ((snd_pcm_uframes_t)io_hw_ptr >= pcm->io_hw_boundary)
			io_hw_ptr -= pcm->io_hw_boundary;

		/* When used with the rate plugin the buffer size may not be an
		 * integer multiple of the period size. If so, the current period may
		 * be split, part at the end of the buffer and the remainder at the
		 * start. In this case we must perform the transfer in two chunks to
		 * make up a full period.  */
		snd_pcm_uframes_t chunk = frames;
		if (io->buffer_size - offset < frames)
			chunk = io->buffer_size - offset;

		snd_pcm_uframes_t frames_transfered = 0;
		while (frames_transfered < frames) {

			/* IO operation size in bytes */
			size_t len = chunk * pcm->frame_size;
			char *head = pcm->io_hw_buffer + offset * pcm->frame_size;

			ssize_t ret = 0;
			if (io->stream == SND_PCM_STREAM_CAPTURE) {

				/* Read the whole chunk "atomically". This will assure, that
				 * frames are not fragmented, so the pointer can be correctly
				 * updated. */
				while (len != 0 && (ret = read(pcm->ba_pcm_fd, head, len)) != 0) {
					if (ret == -1) {
						if (errno == EINTR)
							continue;
						SNDERR("PCM FIFO read error: %s", strerror(errno));
						pcm->connected = false;
						goto fail;
					}
					head += ret;
					len -= ret;
				}

				if (ret == 0) {
					pcm->connected = false;
					goto fail;
				}

			}
			else {

				/* Perform atomic write - see the explanation above. */
				do {
					if ((ret = write(pcm->ba_pcm_fd, head, len)) == -1) {
						if (errno == EINTR)
							continue;
						if (errno != EPIPE)
							SNDERR("PCM FIFO write error: %s", strerror(errno));
						pcm->connected = false;
						goto fail;
					}
					head += ret;
					len -= ret;
				} while (len != 0);

			}

			frames_transfered += chunk;
			offset = 0;
			chunk = frames - chunk;

		}

		io_thread_update_delay(pcm, io_hw_ptr);

		/* synchronize playback time */
		if (io->stream == SND_PCM_STREAM_PLAYBACK)
			asrsync_sync(&asrs, frames);

		/* Make the new HW pointer value visible to the ioplug. */
		pcm->io_hw_ptr = io_hw_ptr;

		/* Wake application thread if enough space/frames is available. */
		if (frames + io->buffer_size - avail >= pcm->io_avail_min)
			eventfd_write(pcm->event_fd, 1);

	}

fail:

	/* make sure we will not get stuck in the pause sync loop */
	pthread_mutex_lock(&pcm->mutex);
	pcm->pause_state = BA_PAUSE_STATE_PAUSED;
	pthread_mutex_unlock(&pcm->mutex);
	pthread_cond_signal(&pcm->pause_cond);

	eventfd_write(pcm->event_fd, 0xDEAD0000);

	/* wait for cancellation from main thread */
	while (true)
		sleep(3600);

	pthread_cleanup_pop(1);
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

	if (!ba_dbus_pcm_ctrl_send_resume(pcm->ba_pcm_ctrl_fd, NULL)) {
		debug2("Couldn't start PCM: %s", strerror(errno));
		return -EIO;
	}

	/* Initialize delay calculation - capture reception begins immediately,
	 * playback transmission begins only when first period has been written
	 * by the application. */
	pcm->delay_running = io->stream == SND_PCM_STREAM_CAPTURE ? true : false;
	gettimestamp(&pcm->delay_ts);

	/* start the IO thread */
	pcm->io_started = true;
	if ((errno = pthread_create(&pcm->io_thread, NULL,
					PTHREAD_FUNC(io_thread), io)) != 0) {
		debug2("Couldn't create IO thread: %s", strerror(errno));
		pcm->io_started = false;
		return -EIO;
	}

	pthread_setname_np(pcm->io_thread, "pcm-io");
	return 0;
}

static int bluealsa_stop(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Stopping");

	io_thread_cancel(pcm);

	pcm->delay_running = false;
	pcm->delay_pcm_nread = 0;

	/* Bug in ioplug - if pcm->io_hw_ptr == -1 then it reports state
	 * SND_PCM_STATE_XRUN instead of SND_PCM_STATE_SETUP after PCM
	 * was stopped. */
	pcm->io_hw_ptr = 0;

	if (!ba_dbus_pcm_ctrl_send_drop(pcm->ba_pcm_ctrl_fd, NULL))
		return -EIO;

	/* Applications that call poll() after snd_pcm_drain() will be blocked
	 * forever unless we generate a poll() event here. */
	eventfd_write(pcm->event_fd, 1);

	return 0;
}

static snd_pcm_sframes_t bluealsa_pointer(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* Any error returned here is translated to -EPIPE, SND_PCM_STATE_XRUN,
	 * by ioplug; and that prevents snd_pcm_readi() and snd_pcm_writei()
	 * from returning -ENODEV to the application on device disconnection.
	 * Instead, when the device is disconnected, we update the PCM state
	 * directly here but we do not return an error code. This ensures that
	 * ioplug does not undo that state change. Both snd_pcm_readi() and
	 * snd_pcm_writei() return -ENODEV when the PCM state is
	 * SND_PCM_STATE_DISCONNECTED after their internal call to
	 * snd_pcm_avail_update(), which will be the case when we set it here.
	 */
	if (!pcm->connected)
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);

#ifndef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
	if (pcm->io_hw_ptr != -1)
		return pcm->io_hw_ptr % io->buffer_size;
#endif
	return pcm->io_hw_ptr;
}

static int bluealsa_close(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Closing");
	ba_dbus_connection_ctx_free(&pcm->dbus_ctx);
	if (pcm->event_fd != -1)
		close(pcm->event_fd);
	pthread_mutex_destroy(&pcm->mutex);
	pthread_cond_destroy(&pcm->pause_cond);
	free(pcm);
	return 0;
}

#if BLUEALSA_HW_PARAMS_FIX
/**
 * Substitute the period and buffer size produced by the ioplug hw param
 * refinement algorithm with values that do not trigger the rate plugin
 * avail() implementation bug.
 *
 * It is not possible to expand the configuration within a hw_params
 * container, only to narrow it. By the time we get to see the container
 * it has already been reduced to a single configuration, so is effectively
 * read-only. So in order to fix the problematic buffer size calculated by
 * the ioplug, we need to completely replace the hw_params container for
 * the bluealsa pcm.
 * */
static int bluealsa_fix_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
#if DEBUG
	struct bluealsa_pcm *pcm = io->private_data;
#endif
	int ret = 0;

	snd_pcm_uframes_t period_size;
	if ((ret = snd_pcm_hw_params_get_period_size(params, &period_size, 0)) < 0)
		return ret;
	snd_pcm_uframes_t buffer_size;
	if ((ret =snd_pcm_hw_params_get_buffer_size(params, &buffer_size)) < 0)
		return ret;

	if (buffer_size % period_size == 0)
		return 0;

	debug2("Attempting to fix hw params buffer size");

	snd_pcm_hw_params_t *refined_params;

	snd_pcm_hw_params_alloca(&refined_params);
	if ((ret = snd_pcm_hw_params_any(io->pcm, refined_params)) < 0)
		return ret;

	snd_pcm_access_mask_t *access = alloca(snd_pcm_access_mask_sizeof());
	if ((ret = snd_pcm_hw_params_get_access_mask(params, access)) < 0)
		return ret;
	if ((ret = snd_pcm_hw_params_set_access_mask(io->pcm, refined_params, access)) < 0)
		return ret;

	snd_pcm_format_t format;
	if ((ret = snd_pcm_hw_params_get_format(params, &format)) < 0)
		return ret;
	if ((ret = snd_pcm_hw_params_set_format(io->pcm, refined_params, format)) < 0)
		return ret;

	unsigned int channels;
	if ((ret = snd_pcm_hw_params_get_channels(params, &channels)) < 0)
		return ret;
	if ((ret = snd_pcm_hw_params_set_channels(io->pcm, refined_params, channels)) < 0)
		return ret;

	unsigned int rate;
	if ((ret = snd_pcm_hw_params_get_rate(params, &rate, 0)) < 0)
		return ret;
	if ((ret = snd_pcm_hw_params_set_rate(io->pcm, refined_params, rate, 0)) < 0)
		return ret;

	if ((ret = snd_pcm_hw_params_set_period_size(io->pcm, refined_params, period_size, 0)) < 0)
		return ret;

	if ((ret = snd_pcm_hw_params_set_periods_integer(io->pcm, refined_params)) < 0)
		return ret;

	buffer_size = (buffer_size / period_size) * period_size;
	if ((ret = snd_pcm_hw_params_set_buffer_size(io->pcm, refined_params, buffer_size)) < 0)
		return ret;

	snd_pcm_hw_params_copy(params, refined_params);

	return ret;
}
#endif

static int bluealsa_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;

	debug2("Initializing HW");

#if BLUEALSA_HW_PARAMS_FIX
	if (bluealsa_fix_hw_params(io, params) < 0)
		debug2("Warning - unable to fix incorrect buffer size in hw parameters");
#endif

	snd_pcm_uframes_t period_size;
	int ret;
	if ((ret = snd_pcm_hw_params_get_period_size(params, &period_size, 0)) < 0)
		return ret;
	snd_pcm_uframes_t buffer_size;
	if ((ret = snd_pcm_hw_params_get_buffer_size(params, &buffer_size)) < 0)
		return ret;

	pcm->frame_size = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_pcm_open(&pcm->dbus_ctx, pcm->ba_pcm.pcm_path,
				&pcm->ba_pcm_fd, &pcm->ba_pcm_ctrl_fd, &err)) {
		debug2("Couldn't open PCM: %s", err.message);
		ret = -dbus_error_to_errno(&err);
		dbus_error_free(&err);
		return ret;
	}

	pcm->connected = true;

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
	pcm->io_avail_min = period_size;

	debug2("Selected HW buffer: %zd periods x %zd bytes %c= %zd bytes",
			buffer_size / period_size, pcm->frame_size * period_size,
			period_size * (buffer_size / period_size) == buffer_size ? '=' : '<',
			buffer_size * pcm->frame_size);

	return 0;
}

static int bluealsa_hw_free(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Freeing HW");

	/* Before closing PCM transport make sure that
	 * the IO thread is terminated. */
	io_thread_cancel(pcm);

	int rv = 0;
	if (pcm->ba_pcm_fd != -1)
		rv |= close(pcm->ba_pcm_fd);
	if (pcm->ba_pcm_ctrl_fd != -1)
		rv |= close(pcm->ba_pcm_ctrl_fd);

	pcm->ba_pcm_fd = -1;
	pcm->ba_pcm_ctrl_fd = -1;
	pcm->connected = false;

	return rv == 0 ? 0 : -errno;
}

static int bluealsa_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Initializing SW");

	snd_pcm_uframes_t boundary;
	snd_pcm_sw_params_get_boundary(params, &boundary);
	pcm->io_hw_boundary = boundary;

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
	if (!pcm->connected) {
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

	if (io->stream == SND_PCM_STREAM_PLAYBACK) {
		/* Indicate that our PCM is ready for IO, even though is is not 100%
		 * true - the IO thread may not be running yet. Applications using
		 * snd_pcm_sw_params_set_start_threshold() require the PCM to be usable
		 * as soon as it has been prepared. */
		if (pcm->io_avail_min < io->buffer_size)
			eventfd_write(pcm->event_fd, 1);
	}
	else {
		/* Make sure there is no poll event still pending (for example when
		 * preparing after an overrun). */
			eventfd_t event;
			eventfd_read(pcm->event_fd, &event);
	}

	debug2("Prepared");
	return 0;
}

static int bluealsa_drain(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug2("Draining");

	if (!pcm->connected) {
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);
		return -ENODEV;
	}

	/* A bug in the ioplug drain implementation means that snd_pcm_drain()
	 * always either finishes in state SND_PCM_STATE_SETUP or returns an error.
	 * It is not possible to finish in state SND_PCM_STATE_DRAINING and return
	 * success; therefore is is impossible to correctly implement capture
	 * drain logic. So for capture PCMs we do nothing and return success;
	 * ioplug will stop the PCM. */
	if (io->stream == SND_PCM_STREAM_CAPTURE)
		return 0;

	/* We must ensure that all remaining frames in the ring buffer are flushed
	 * to the FIFO by the I/O thread. It is possible that the client has called
	 * snd_pcm_drain() without the start_threshold having been reached, or
	 * while paused, so we must first ensure that the IO thread is running. */
	if (bluealsa_start(io) < 0) {
		/* Insufficient resources to start a new thread - so we have no choice
		 * but to drop this stream. */
		bluealsa_stop(io);
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_SETUP);
		return -EIO;
	}

	/* For a non-blocking drain, we do not wait for the drain to complete. */
	if (io->nonblock)
		return -EAGAIN;

	struct pollfd pfd = { pcm->event_fd, POLLIN, 0 };
	bool aborted = false;
	int ret = 0;

	snd_pcm_sframes_t hw_ptr;
	while ((hw_ptr = bluealsa_pointer(io)) >= 0 && io->state == SND_PCM_STATE_DRAINING) {

		snd_pcm_uframes_t avail = snd_pcm_ioplug_hw_avail(io, hw_ptr, io->appl_ptr);

		/* If the buffer is empty then the local drain is complete. */
		if (avail == 0)
			break;

		/* We set a timeout to ensure that the plugin cannot block forever in
		 * case the server has stopped reading from the FIFO. Allow enough time
		 * to drain the available frames as full periods, plus 100 ms:
		 * e.g. one or less periods in buffer, allow 100ms + one period, more
		 *      than one but not more than two, allow 100ms + two periods, etc.
		 * If the wait is re-started after being interrupted by a signal then
		 * we must re-calculate the maximum waiting time that remains. */
		int timeout = 100 + (((avail - 1) / io->period_size) + 1) * io->period_size * 1000 / io->rate;

		int nready = poll(&pfd, 1, timeout);
		if (nready == -1) {
			if (errno == EINTR) {
				/* It is not well documented by ALSA, but if the application has
				 * requested that the PCM should be aborted by a signal then the
				 * ioplug nonblock flag is set to the special value 2. */
				if (io->nonblock != 2)
					continue;
				/* Application has aborted the drain. */
				debug2("Drain aborted by signal");
				aborted = true;
			}
			else {
				debug2("Drain poll error: %s", strerror(errno));
				bluealsa_stop(io);
				snd_pcm_ioplug_set_state(io, SND_PCM_STATE_SETUP);
				ret = -EIO;
			}

			break;
		}
		if (nready == 0) {
			/* Timeout - do not wait any longer. */
			SNDERR("Drain timed out: Possible Bluetooth transport failure");
			bluealsa_stop(io);
			io->state = SND_PCM_STATE_SETUP;
			ret = -EIO;
			break;
		}

		if (pfd.revents & POLLIN) {
			eventfd_t event;
			eventfd_read(pcm->event_fd, &event);
		}

	}

	if (io->state == SND_PCM_STATE_DRAINING && !aborted)
		if (!ba_dbus_pcm_ctrl_send_drain(pcm->ba_pcm_ctrl_fd, NULL)) {
			bluealsa_stop(io);
			io->state = SND_PCM_STATE_SETUP;
			ret = -EIO;
		}

	return ret;
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
		ba_dbus_connection_dispatch(&pcm->dbus_ctx);
		gettimestamp(&pcm->dbus_dispatch_ts);
	}

	pthread_mutex_lock(&pcm->mutex);

	/* if PCM is not started there should be no capture delay */
	if (!pcm->delay_running && io->stream == SND_PCM_STREAM_CAPTURE) {
		pthread_mutex_unlock(&pcm->mutex);
		return 0;
	}

	struct timespec diff;
	timespecsub(&now, &pcm->delay_ts, &diff);

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
		while (!(pcm->pause_state & BA_PAUSE_STATE_PAUSED) && pcm->connected)
			pthread_cond_wait(&pcm->pause_cond, &pcm->mutex);
		pthread_mutex_unlock(&pcm->mutex);
	}

	if (!pcm->connected) {
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);
		return -ENODEV;
	}

	if (!ba_dbus_pcm_ctrl_send(pcm->ba_pcm_ctrl_fd,
				enable ? "Pause" : "Resume", 200, NULL))
		return -EIO;

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
	snd_output_printf(out, "BlueALSA Bluetooth codec: %s\n", pcm->ba_pcm.codec.name);
	/* alsa-lib commits the PCM setup only if bluealsa_hw_params() returned
	 * success, so we only dump the ALSA PCM parameters if the BlueALSA PCM
	 * connection is established. */
	if (pcm->connected) {
		snd_output_printf(out, "Its setup is:\n");
		snd_pcm_dump_setup(io->pcm, out);
	}
}

static int bluealsa_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (!pcm->connected) {
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
	ba_dbus_connection_poll_fds(&pcm->dbus_ctx, NULL, &dbus_nfds);

	return 1 + dbus_nfds;
}

static int bluealsa_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd,
		unsigned int nfds) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (nfds < 1)
		return -EINVAL;

	nfds_t dbus_nfds = nfds - 1;
	if (!ba_dbus_connection_poll_fds(&pcm->dbus_ctx, &pfd[1], &dbus_nfds))
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

	if (nfds < 1)
		return -EINVAL;

	ba_dbus_connection_poll_dispatch(&pcm->dbus_ctx, &pfd[1], nfds - 1);
	while (dbus_connection_dispatch(pcm->dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
		continue;
	gettimestamp(&pcm->dbus_dispatch_ts);

	if (!pcm->connected)
		goto fail;

	if (pfd[0].revents & POLLIN) {

		eventfd_t event;
		eventfd_read(pcm->event_fd, &event);

		if (event & 0xDEAD0000)
			goto fail;

		/* This call synchronizes the ring buffer pointers and updates the
		 * ioplug state. For non-blocking drains it also causes ioplug to drop
		 * the stream when the buffer is empty. */
		snd_pcm_sframes_t avail = snd_pcm_avail(io->pcm);

		/* ALSA expects that the event will match stream direction, e.g.
		 * playback will not start if the event is for reading. */
		*revents = io->stream == SND_PCM_STREAM_CAPTURE ? POLLIN : POLLOUT;

		/* We hold the event fd ready, unless insufficient frames are
		 * available in the ring buffer. */
		bool ready = true;

		switch (io->state) {
			case SND_PCM_STATE_SETUP:
				/* To support non-blocking drain we must report a POLLOUT event
				 * for playback PCMs here, because the above call to
				 * snd_pcm_avail() may have changed the state to
				 * SND_PCM_STATE_SETUP. */
				if (io->stream == SND_PCM_STREAM_CAPTURE)
					*revents = 0;
				ready = false;
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
			case SND_PCM_STATE_DRAINING:
				/* BlueALSA does not drain capture PCMs. So this state only
				 * occurs with playback PCMs. Do not wake the application until
				 * the buffer is empty. */
				if ((snd_pcm_uframes_t)avail < io->buffer_size) {
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

/**
 * Convert volume string to volume level and mute state.
 *
 * Mute state is determined by the last character of the volume
 * string. The '-' at the end indicates to mute; '+' indicates
 * to unmute. */
static int str2volume(const char *str, int *volume, int *mute) {

	char *endptr;
	long v = strtol(str, &endptr, 10);
	if (endptr != str) {
		if (v < 0 || v > 100)
			return -1;
		*volume = v;
	}

	if (endptr[0] == '+' && endptr[1] == '\0')
		*mute = 0;
	else if (endptr[0] == '-' && endptr[1] == '\0')
		*mute = 1;
	else if (endptr[0] != '\0')
		return -1;

	return 0;
}

static int str2softvol(const char *str) {
	return snd_config_get_bool_ascii(str);
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
		dbus_message_iter_get_ba_pcm_props(&iter, NULL, &pcm->ba_pcm);

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

static bool bluealsa_select_pcm_codec(struct bluealsa_pcm *pcm, const char *codec, DBusError *err) {

	char name[32] = { 0 };
	size_t name_len = sizeof(name) - 1;
	uint8_t config[64] = { 0 };
	ssize_t config_len = 0;

	const char *config_hex;
	/* split the given string into name and configuration components */
	if ((config_hex = strchr(codec, ':')) != NULL) {
		name_len = MIN(name_len, (size_t)(config_hex - codec));
		config_hex++;

		size_t config_hex_len;
		if ((config_hex_len = strlen(config_hex)) > sizeof(config) * 2) {
			dbus_set_error(err, DBUS_ERROR_FAILED, "Invalid codec configuration: %s", config_hex);
			return false;
		}

		if ((config_len = hex2bin(config_hex, config, config_hex_len)) == -1) {
			dbus_set_error(err, DBUS_ERROR_FAILED, "%s", strerror(errno));
			return false;
		}

	}

	strncpy(name, codec, name_len);
	if (!ba_dbus_pcm_select_codec(&pcm->dbus_ctx, pcm->ba_pcm.pcm_path,
				ba_dbus_pcm_codec_get_canonical_name(name), config, config_len, 0, err))
		return false;

	return true;
}

static bool bluealsa_update_pcm_volume(struct bluealsa_pcm *pcm,
		int volume, int mute, DBusError *err) {
	uint16_t old = pcm->ba_pcm.volume.raw;
	if (volume >= 0) {
		const int v = BA_PCM_VOLUME_MAX(&pcm->ba_pcm) * volume / 100;
		pcm->ba_pcm.volume.ch1_volume = v;
		if (pcm->ba_pcm.channels == 2)
			pcm->ba_pcm.volume.ch2_volume = v;
	}
	if (mute >= 0) {
		const bool v = !!mute;
		pcm->ba_pcm.volume.ch1_muted = v;
		if (pcm->ba_pcm.channels == 2)
			pcm->ba_pcm.volume.ch2_muted = v;
	}
	if (pcm->ba_pcm.volume.raw == old)
		return true;
	return ba_dbus_pcm_update(&pcm->dbus_ctx, &pcm->ba_pcm, BLUEALSA_PCM_VOLUME, err);
}

static bool bluealsa_update_pcm_softvol(struct bluealsa_pcm *pcm,
		int softvol, DBusError *err) {
	if (softvol < 0 || !!softvol == pcm->ba_pcm.soft_volume)
		return true;
	pcm->ba_pcm.soft_volume = !!softvol;
	return ba_dbus_pcm_update(&pcm->dbus_ctx, &pcm->ba_pcm, BLUEALSA_PCM_SOFT_VOLUME, err);
}

SND_PCM_PLUGIN_DEFINE_FUNC(bluealsa) {
	(void)root;

	const char *service = BLUEALSA_SERVICE;
	const char *device = NULL;
	const char *profile = NULL;
	const char *codec = NULL;
	const char *volume = NULL;
	const char *softvol = NULL;
	long delay = 0;
	struct bluealsa_pcm *pcm;
	int ret;

	snd_config_iterator_t pos, next;
	snd_config_for_each(pos, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(pos);

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
		if (strcmp(id, "codec") == 0) {
			if (snd_config_get_string(n, &codec) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			if (strcmp(codec, "unchanged") == 0)
				codec = NULL;
			continue;
		}
		if (strcmp(id, "volume") == 0) {
			if (snd_config_get_string(n, &volume) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			if (strcmp(volume, "unchanged") == 0)
				volume = NULL;
			continue;
		}
		if (strcmp(id, "softvol") == 0) {
			if (snd_config_get_string(n, &softvol) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			if (strcmp(softvol, "unchanged") == 0)
				softvol = NULL;
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

	int pcm_mute = -1;
	int pcm_volume = -1;
	if (volume != NULL && str2volume(volume, &pcm_volume, &pcm_mute) != 0) {
		SNDERR("Invalid volume [0-100][+-]: %s", volume);
		return -EINVAL;
	}

	int pcm_softvol = -1;
	if (softvol != NULL && (pcm_softvol = str2softvol(softvol)) < 0) {
		SNDERR("Invalid softvol: %s", softvol);
		return -EINVAL;
	}

	if ((pcm = calloc(1, sizeof(*pcm))) == NULL)
		return -ENOMEM;

	pcm->io.version = SND_PCM_IOPLUG_VERSION;
	pcm->io.name = "BlueALSA";
	pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
#ifdef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
	pcm->io.flags |= SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;
#endif
	pcm->io.mmap_rw = 1;
	pcm->io.callback = &bluealsa_callback;
	pcm->io.private_data = pcm;

	pcm->event_fd = -1;
	pcm->ba_pcm_fd = -1;
	pcm->ba_pcm_ctrl_fd = -1;
	pcm->delay_ex = delay;
	pthread_mutex_init(&pcm->mutex, NULL);
	pthread_cond_init(&pcm->pause_cond, NULL);
	pcm->pause_state = BA_PAUSE_STATE_RUNNING;

	dbus_threads_init_default();

	DBusError err = DBUS_ERROR_INIT;
	if (ba_dbus_connection_ctx_init(&pcm->dbus_ctx, service, &err) != TRUE) {
		SNDERR("Couldn't initialize D-Bus context: %s", err.message);
		ret = -dbus_error_to_errno(&err);
		goto fail;
	}

	if (!dbus_connection_add_filter(pcm->dbus_ctx.conn, bluealsa_dbus_msg_filter, pcm, NULL)) {
		SNDERR("Couldn't add D-Bus filter: %s", strerror(ENOMEM));
		ret = -ENOMEM;
		goto fail;
	}

	debug("Getting BlueALSA PCM: %s %s %s", snd_pcm_stream_name(stream), device, profile);
	if (!ba_dbus_pcm_get(&pcm->dbus_ctx, &ba_addr, ba_profile,
				stream == SND_PCM_STREAM_PLAYBACK ? BA_PCM_MODE_SINK : BA_PCM_MODE_SOURCE,
				&pcm->ba_pcm, &err)) {
		SNDERR("Couldn't get BlueALSA PCM: %s", err.message);
		ret = -dbus_error_to_errno(&err);
		goto fail;
	}

	/* Subscribe for properties-changed signals but for the opened PCM only. */
	ba_dbus_connection_signal_match_add(&pcm->dbus_ctx, pcm->dbus_ctx.ba_service,
			pcm->ba_pcm.pcm_path, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged",
			"arg0='"BLUEALSA_INTERFACE_PCM"'");

	if ((pcm->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) == -1) {
		ret = -errno;
		goto fail;
	}

	if (codec != NULL && codec[0] != '\0') {
		if (bluealsa_select_pcm_codec(pcm, codec, &err)) {
			/* Changing the codec may change the audio format, sampling rate and/or
			 * channels. We need to refresh our cache of PCM properties. */
			if (!ba_dbus_pcm_get(&pcm->dbus_ctx, &ba_addr, ba_profile,
						stream == SND_PCM_STREAM_PLAYBACK ? BA_PCM_MODE_SINK : BA_PCM_MODE_SOURCE,
						&pcm->ba_pcm, &err)) {
				SNDERR("Couldn't get BlueALSA PCM: %s", err.message);
				ret = -dbus_error_to_errno(&err);
				goto fail;
			}
		}
		else {
			SNDERR("Couldn't select BlueALSA PCM codec: %s", err.message);
			dbus_error_free(&err);
		}
	}

	/* If the BT transport codec is not known (which means that PCM sampling
	 * rate is also not know), we cannot construct useful constraints. */
	if (pcm->ba_pcm.sampling == 0) {
		ret = -EAGAIN;
		goto fail;
	}

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
		goto fail;
	}

	if (!bluealsa_update_pcm_softvol(pcm, pcm_softvol, &err)) {
		SNDERR("Couldn't set BlueALSA PCM soft-volume: %s", err.message);
		dbus_error_free(&err);
	}

	if (!bluealsa_update_pcm_volume(pcm, pcm_volume, pcm_mute, &err)) {
		SNDERR("Couldn't set BlueALSA PCM volume: %s", err.message);
		dbus_error_free(&err);
	}

	*pcmp = pcm->io.pcm;
	return 0;

fail:
	bluealsa_close(&pcm->io);
	dbus_error_free(&err);
	return ret;
}

SND_PCM_PLUGIN_SYMBOL(bluealsa)
