/*
 * BlueALSA - sco.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "sco.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sco.h>

#include <glib.h>

#include "audio.h"
#include "ba-device.h"
#include "ba-transport-pcm.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#if ENABLE_MSBC
# include "codec-msbc.h"
#endif
#include "codec-sbc.h"
#include "hci.h"
#include "hfp.h"
#include "io.h"
#include "shared/bluetooth.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

/**
 * SCO dispatcher internal data. */
struct sco_data {
	struct ba_adapter *a;
	struct pollfd pfd;
};

static void sco_dispatcher_cleanup(struct sco_data *data) {
	debug("SCO dispatcher cleanup: %s", data->a->hci.name);
	if (data->pfd.fd != -1)
		close(data->pfd.fd);
}

static void *sco_dispatcher_thread(struct ba_adapter *a) {

	struct sco_data data = { .a = a, .pfd = { -1, POLLIN, 0 } };

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(sco_dispatcher_cleanup), &data);

	sigset_t sigset;
	/* See the ba_transport_pcm_start() function for information
	 * why we have to mask all signals. */
	sigfillset(&sigset);
	pthread_sigmask(SIG_SETMASK, &sigset, NULL);

	if ((data.pfd.fd = hci_sco_open(data.a->hci.dev_id)) == -1) {
		error("Couldn't open SCO socket: %s", strerror(errno));
		goto fail;
	}

#if ENABLE_MSBC
	uint32_t defer = 1;
	if (setsockopt(data.pfd.fd, SOL_BLUETOOTH, BT_DEFER_SETUP, &defer, sizeof(defer)) == -1) {
		error("Couldn't set deferred connection setup: %s", strerror(errno));
		goto fail;
	}
#endif

	if (listen(data.pfd.fd, 10) == -1) {
		error("Couldn't listen on SCO socket: %s", strerror(errno));
		goto fail;
	}

	debug("Starting SCO dispatcher loop: %s", a->hci.name);
	for (;;) {

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		int poll_rv = poll(&data.pfd, 1, -1);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (poll_rv == -1) {
			if (errno == EINTR)
				continue;
			error("SCO dispatcher poll error: %s", strerror(errno));
			goto fail;
		}

		struct sockaddr_sco addr;
		socklen_t addrlen = sizeof(addr);
		struct ba_device *d = NULL;
		struct ba_transport *t = NULL;
		char addrstr[18];
		int fd = -1;

		if ((fd = accept(data.pfd.fd, (struct sockaddr *)&addr, &addrlen)) == -1) {
			error("Couldn't accept incoming SCO link: %s", strerror(errno));
			goto cleanup;
		}

		ba2str(&addr.sco_bdaddr, addrstr);
		debug("New incoming SCO link: %s: %d", addrstr, fd);

		if ((d = ba_device_lookup(data.a, &addr.sco_bdaddr)) == NULL) {
			error("Couldn't lookup device: %s", addrstr);
			goto cleanup;
		}

		if ((t = ba_transport_lookup(d, d->bluez_dbus_path)) == NULL) {
			error("Couldn't lookup transport: %s", d->bluez_dbus_path);
			goto cleanup;
		}

#if ENABLE_MSBC
		struct bt_voice voice = { .setting = BT_VOICE_TRANSPARENT };
		if (ba_transport_get_codec(t) == HFP_CODEC_MSBC &&
				setsockopt(fd, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof(voice)) == -1) {
			error("Couldn't setup transparent voice: %s", strerror(errno));
			goto cleanup;
		}
		if (read(fd, &voice, 1) == -1) {
			error("Couldn't authorize SCO connection: %s", strerror(errno));
			goto cleanup;
		}
#endif

		ba_transport_stop(t);

		pthread_mutex_lock(&t->bt_fd_mtx);

		t->bt_fd = fd;
		t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd);

		fd = -1;

		pthread_mutex_unlock(&t->bt_fd_mtx);

		ba_transport_thread_state_set_idle(&t->thread_enc);
		ba_transport_thread_state_set_idle(&t->thread_dec);
		ba_transport_start(t);

cleanup:
		if (d != NULL)
			ba_device_unref(d);
		if (t != NULL)
			ba_transport_unref(t);
		if (fd != -1)
			close(fd);

	}

fail:
	pthread_cleanup_pop(1);
	return NULL;
}

int sco_setup_connection_dispatcher(struct ba_adapter *a) {

	/* skip setup if dispatcher thread is already running */
	if (!pthread_equal(a->sco_dispatcher, config.main_thread))
		return 0;

	/* XXX: It is a known issue with Broadcom chips, that by default, the SCO
	 *      packets are routed via the chip's PCM interface. However, the IO
	 *      thread expects data to be available via the transport interface. */
	if (a->chip.manufacturer == BT_COMPID_BROADCOM) {

		int dd;
		uint8_t routing, clock, frame, sync, clk;

		debug("Checking Broadcom internal SCO routing");

		if ((dd = hci_open_dev(a->hci.dev_id)) == -1 ||
				hci_bcm_read_sco_pcm_params(dd, &routing, &clock, &frame, &sync, &clk, 1000) == -1)
			error("Couldn't read SCO routing params: %s", strerror(errno));
		else {
			debug("Current SCO interface setup: %u %u %u %u %u", routing, clock, frame, sync, clk);
			if (routing != BT_BCM_PARAM_ROUTING_TRANSPORT) {
				debug("Setting SCO routing via transport interface");
				if (hci_bcm_write_sco_pcm_params(dd, BT_BCM_PARAM_ROUTING_TRANSPORT,
						clock, frame, sync, clk, 1000) == -1)
				error("Couldn't write SCO routing params: %s", strerror(errno));
			}
		}

		if (dd != -1)
			hci_close_dev(dd);

	}

	int ret;

	/* Please note, that during the SCO dispatcher thread creation the adapter
	 * is not referenced. It is guaranteed that the adapter will be available
	 * during the whole live-span of the thread, because the thread is canceled
	 * in the adapter cleanup routine. See the ba_adapter_unref() function. */
	if ((ret = pthread_create(&a->sco_dispatcher, NULL,
					PTHREAD_FUNC(sco_dispatcher_thread), a)) != 0) {
		error("Couldn't create SCO dispatcher: %s", strerror(ret));
		a->sco_dispatcher = config.main_thread;
		return -1;
	}

	pthread_setname_np(a->sco_dispatcher, "ba-sco-dispatch");
	debug("Created SCO dispatcher [%s]: %s", "ba-sco-dispatch", a->hci.name);

	return 0;
}

static void *sco_cvsd_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct ba_transport_thread *th = t_pcm->th;
	struct io_poll io = { .timeout = -1 };

	ffb_t buffer = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

	/* The buffer will be resized once the SCO transfer size is known. We
	 * choose a small value initially (10ms) to ensure that we discard as little
	 * audio as possible. */
	if (ffb_init_int16_t(&buffer, 80) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_init;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	ba_transport_thread_state_set_running(th);

	/* The message size of the BT SCO socket */
	uint16_t transfer_bytes = 0;

	/* The number of PCM samples equivalent to one BT transfer. */
	unsigned int transfer_samples = 0;

	/* use blocking I/O on FIFO until SCO transfer size is known. */
	ssize_t samples = 0;
	size_t start_threshold = SIZE_MAX;
	bool mtu_optimized = false;

	asrsync_init(&io.asrs, t_pcm->sampling);

	for (;;) {
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, buffer.tail, ffb_len_in(&buffer))) {
		case -1:
			if (errno == ESTALE) {
				ffb_rewind(&buffer);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		ffb_seek(&buffer, samples);

		if (!mtu_optimized) {
			transfer_bytes = t->sco.transfer_bytes;
			if (transfer_bytes > 0) {
				debug("SCO message size %d: %d", th->bt_fd, transfer_bytes);
				mtu_optimized = true;
				transfer_samples = transfer_bytes / sizeof(uint16_t);
				start_threshold = transfer_samples + 40;
			}
		}

		t_pcm->delay = ffb_len_out(&buffer);

		if (ffb_len_out(&buffer) > start_threshold)
			break;

		/* Ensure we have room to read at least 10ms on next iteration */
		if (ffb_len_in(&buffer) < 80)
			ffb_shift(&buffer, 80);

		/* keep data transfer at a constant bit rate */
		asrsync_sync(&io.asrs, samples);

	}

	/* We can now safely resize the pcm buffer to optimize read performance */
	size_t bufsize = transfer_samples * 4;
	if (buffer.nmemb < bufsize)
		ffb_init_int16_t(&buffer, bufsize);

	for (;;) {

		samples = ffb_len_out(&buffer);

		const int16_t *input = buffer.data;
		size_t input_samples = samples;

		while (input_samples >= transfer_samples) {

			ssize_t ret;
			if ((ret = io_bt_write(th, input, transfer_bytes)) <= 0) {
				if (ret == -1)
					error("BT write error: %s", strerror(errno));
				goto exit;
			}

			input += transfer_samples;
			input_samples -= transfer_samples;

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, transfer_samples);
		}

		ffb_shift(&buffer, samples - input_samples);

		while ((samples = io_poll_and_read_pcm(&io, t_pcm, buffer.tail, ffb_len_in(&buffer))) <= 0) {
			if (samples == -1) {
				if (errno == ESTALE) {
					ffb_rewind(&buffer);
					continue;
				}
				error("PCM poll and read error: %s", strerror(errno));
			}
			ba_transport_stop_if_no_clients(t);
		}

		ffb_seek(&buffer, samples);
	}

exit:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_init:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static void *sco_cvsd_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct ba_transport_thread *th = t_pcm->th;

	/* SCO transport should deliver audio packets as soon as it is acquired.
	* However, if the adapter does not route incoming SCO to the HCI we will
	* never see those packets. The encoder thread is blocked until this thread
	* signals that the first packet has been received so we set a timeout for
	* the first read only to ensure we can still send that signal. HFP/HSP
	* specs do not define a maximum latency for CVSD, so we choose an arbitrary
	* timeout of 100 ms. */
	struct io_poll io = { .timeout = 100 };

	const size_t mtu_samples = t->mtu_read / sizeof(int16_t);
	const size_t mtu_samples_multiplier = 2;

	ffb_t buffer = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

	if (ffb_init_int16_t(&buffer, mtu_samples * mtu_samples_multiplier) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	bool mtu_optimized = false;

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_thread_state_set_running(th);;) {

		ssize_t len = ffb_blen_in(&buffer);
		if ((len = io_poll_and_read_bt(&io, th, buffer.tail, len)) == -1) {
			if (errno == ETIME) {
				debug("SCO decoder timeout");
				io.timeout = -1;
				/* Use socket SCO options MTU */
				len = (ssize_t) t->mtu_write;
				/* The MTU value returned by kernel btusb driver is incorrect.
				* We use the USB Alt-2 setting of 48 bytes which is correct in
				* the typical case of a single SCO connection in use at a time.
				*/
				if ((t->d->a->hci.type & 0x0F) == HCI_USB) {
					len = 48;
					debug("USB adjusted SCO MTU: %d: %zd", th->bt_fd, len);
				}
				t->sco.transfer_bytes = (unsigned int) len;
				mtu_optimized = true;
				continue;
			}
			error("BT poll and read error: %s", strerror(errno));

		}
		else if (len == 0)
			goto exit;
		else if (!mtu_optimized) {
			t->sco.transfer_bytes = (unsigned int) len;
			io.timeout = -1;
			mtu_optimized = true;
		}

		if ((size_t)len == buffer.nmemb * buffer.size) {
			debug("Resizing CVSD read buffer: %zd -> %zd",
					buffer.nmemb * buffer.size, buffer.nmemb * 2 * buffer.size);
			if (ffb_init_int16_t(&buffer, buffer.nmemb * 2) == -1)
				error("Couldn't resize CVSD read buffer: %s", strerror(errno));
		}

		if (!ba_transport_pcm_is_active(t_pcm))
			continue;

		if (len > 0)
			ffb_seek(&buffer, len / buffer.size);

		ssize_t samples;
		if ((samples = ffb_len_out(&buffer)) <= 0)
			continue;

		io_pcm_scale(t_pcm, buffer.data, samples);
		if ((samples = io_pcm_write(t_pcm, buffer.data, samples)) == -1)
			error("FIFO write error: %s", strerror(errno));
		else if (samples == 0)
			ba_transport_stop_if_no_clients(t);

		ffb_shift(&buffer, samples);

	}

exit:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

#if ENABLE_MSBC
/**
 * Handle all pending transport thread signals (non-blocking).
 */
static enum ba_transport_thread_signal sco_msbc_poll_transport_thread_signals(struct ba_transport_pcm *pcm) {

	struct ba_transport_thread *th = pcm->th;
	struct pollfd pfd = { th->pipe[0], POLLIN, 0 };
	enum ba_transport_thread_signal ret = BA_TRANSPORT_THREAD_SIGNAL_PING;

	for (;;) {
		int err;
		if ((err = poll(&pfd, 1, 0)) == 0)
			break;
		else if (err == -1) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (pfd.revents & POLLIN) {
			enum ba_transport_thread_signal signal;
			ba_transport_thread_signal_recv(th, &signal);
			switch (signal) {
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_SYNC:
				ret = signal;
				break;
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_DROP:
				return signal;
			default:
				break;
			}
		}
	}

	return ret;
}
#endif

#if ENABLE_MSBC

/**
 * Read data from the PCM FIFO (non-blocking).
 *
 * @return on success number of samples read, 0 if none are available.
 *         on failure or FIFO closed -1.
 */
static ssize_t sco_msbc_read_pcm(
		struct ba_transport_pcm *pcm,
		void *buffer,
		size_t samples) {

	ssize_t samples_read;
	if ((samples_read = io_pcm_read(pcm, buffer, samples)) <= 0) {
		if (samples_read == 0) {
			samples_read = -1;
			errno = EBADF;
		}
		else if (errno == EAGAIN)
			samples_read = 0;
		else if (errno != EBADF)
			error("PCM read error: %s", strerror(errno));
	}

	return samples_read;
}
#endif

#if ENABLE_MSBC

/**
 * SCO over a USB HCI requires strict timing of audio data packets to meet the
 * demands of a USB isochronous endpoint. This is crucial for mSBC streams
 * because the USB packet boundaries do not align with the mSBC frame
 * boundaries. So to support USB devices correctly we use the delay timer to
 * regulate writes to the BT socket, and make all other I/O non-blocking to
 * ensure that we do not miss a USB deadline.
 */
static void *sco_msbc_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct ba_transport_thread *th = t_pcm->th;
	/* use blocking I/O on FIFO until SCO transfer size is known. */
	struct io_poll io = { .timeout = -1 };

	/* The message size of the BT SCO socket */
	uint16_t transfer_bytes = 0;
	/* The number of PCM samples equivalent to one BT transfer. */
	unsigned int transfer_samples = 0;
	/* The number of samples required to guarantee that one BT transfer can be
	 * produced. */
	unsigned int pcm_threshold = 0;
	/* The minimum fill level of the PCM buffer to avoid underrun. */
	size_t start_threshold = SIZE_MAX;
	ssize_t samples = 0;

	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);

	if (msbc_init(&msbc) != 0) {
		error("Couldn't initialize mSBC codec: %s", strerror(errno));
		goto fail_msbc;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	ba_transport_thread_state_set_running(th);

	bool mtu_optimized = false;

	asrsync_init(&io.asrs, t_pcm->sampling);

	for (;;) {

		if ((samples = io_poll_and_read_pcm(&io, t_pcm,
						msbc.pcm.tail, ffb_len_in(&msbc.pcm))) <= 0) {
			if (samples == -1) {
				if (errno == ESTALE) {
					/* reinitialize mSBC encoder */
					msbc_init(&msbc);
					continue;
				}
				error("PCM poll and read error: %s", strerror(errno));
			}
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		ffb_seek(&msbc.pcm, samples);
		t_pcm->delay = ffb_len_out(&msbc.pcm) * 10000 / t_pcm->sampling;

		if (!mtu_optimized) {
			transfer_bytes = t->sco.transfer_bytes;
			if (transfer_bytes > 0) {
				debug("SCO message size %d: %d", th->bt_fd, transfer_bytes);
				mtu_optimized = true;
				transfer_samples = transfer_bytes * MSBC_CODESAMPLES / sizeof(struct esco_msbc_frame);
				/* The number of msbc frames required to guarantee that one BT
				 * transfer is available. */
				unsigned int msbc_frames = 1;
				while (msbc_frames * sizeof(struct esco_msbc_frame) < transfer_bytes)
					msbc_frames++;
				pcm_threshold = msbc_frames * MSBC_CODESAMPLES;

				start_threshold = pcm_threshold + transfer_samples;
			}
		}

		/* keep data transfer at a constant bit rate */
		asrsync_sync(&io.asrs, samples);

		if ((size_t)(samples = ffb_len_out(&msbc.pcm)) >= start_threshold) {
			/* To minimize latency, discard oldest samples to reduce buffered
			 * quantity to the start_threshold. */
			ffb_shift(&msbc.pcm, samples - start_threshold);
			t_pcm->delay = start_threshold * 10000 / t_pcm->sampling;
			break;
		}

		/* Ensure we have room to read at least 10ms on next iteration */
		if (ffb_len_in(&msbc.pcm) < 160)
			ffb_shift(&msbc.pcm, 160);

	}

	/* We now regulate the rate at which transfers to the SCO socket occur, so
	 * we must reset the timer. */
	asrsync_init(&io.asrs, t_pcm->sampling);

	bool stopping = false;
	bool draining = false;
	size_t drain_samples = 0;

	for (;;) {

		/* Encode sufficient samples to guarantee the next transfer. We have
		 * guaranteed that enough samples are available before reaching this
		 * point. */
		while (ffb_len_out(&msbc.data) < transfer_bytes) {
			int err;
			if ((err = msbc_encode(&msbc)) < 0) {
				error("mSBC encoding error: %s", sbc_strerror(err));
				goto exit;
			}
		}

		/* Write out the next transfer. */
		uint8_t *data = msbc.data.data;
		size_t len;
		ssize_t written = 0;
		for (len = transfer_bytes; len > 0; data += written, len -= written) {
			if ((written = io_bt_write(th, data, len)) <= 0) {
				if (written == -1)
					error("BT write error: %s", strerror(errno));
				goto exit;
			}
		}

		/* Move unprocessed data to the front of our linear buffer. */
		ffb_shift(&msbc.data, transfer_bytes);
		msbc.frames = 0;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		/* keep data transfer at a constant bit rate. This is the only thread
		 * cancellation point within the encoder loop */
		asrsync_sync(&io.asrs, transfer_samples);

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* Check if any control commands were received while we were sleeping */
		switch (sco_msbc_poll_transport_thread_signals(t_pcm)) {
		case BA_TRANSPORT_THREAD_SIGNAL_PCM_DROP:
			io_pcm_flush(t_pcm);
			/* Rewinding the msbc data buffer may result in an incomplete frame
			 * having been sent. So to maintain the integrity of the stream we
			 * allow already encoded audio to play out, and only discard the
			 * un-encoded PCM samples. */
			ffb_rewind(&msbc.pcm);
			draining = false;
			break;
		case BA_TRANSPORT_THREAD_SIGNAL_PCM_SYNC:
			if (!draining) {
				/* Make a note of how much audio is buffered at the time the
				 * DRAIN request is received. We will signal the transport
				 * thread when this number of samples has been written to BT. */
				draining = true;
				drain_samples = io.asrs.frames + ffb_len_out(&msbc.pcm) + 2 * ffb_len_out(&msbc.data);
			}
			break;
		default:
			break;
		}

		if (ffb_len_in(&msbc.pcm) > 0) {
			struct pollfd pfd[] = {{ t_pcm->fd, POLLIN, 0 }};
			if (poll(pfd, 1, 0) == 1) {
				if ((samples = sco_msbc_read_pcm(t_pcm, msbc.pcm.tail, ffb_len_in(&msbc.pcm))) < 0) {
					if (errno == EBADF) {
						if (!stopping)  {
							ba_transport_stop_if_no_clients(t);
							stopping = true;
						}
					}
					samples = 0;
				}
				else {
					stopping = false;
					ffb_seek(&msbc.pcm, samples);

					/* It is possible that we received a DRAIN request before we had
					 * read all samples from the FIFO. So we apply any necessary
					 * adjustment here. */
					if (draining)
						drain_samples += samples;
				}
			}
		}

		size_t pcm_avail = ffb_len_out(&msbc.pcm);
		size_t data_avail = ffb_len_out(&msbc.data);
		if (data_avail < transfer_bytes && pcm_avail < pcm_threshold) {

			/* client has stopped sending audio data, so first check if we have
			 * completed the drain of buffered samples. */
			if (draining && io.asrs.frames >= drain_samples) {
				pthread_mutex_lock(&t_pcm->mutex);
				t_pcm->synced = true;
				pthread_mutex_unlock(&t_pcm->mutex);
				pthread_cond_signal(&t_pcm->cond);
				draining = false;
			}

			/* We must continue to send valid msbc frames until the transport is
			 * released, so we insert silence into the stream. We apply just
			 * enough silence to bring the buffer level up to the
			 * start_threshold to minimize the interruption in case the client
			 * later sends more samples. */
			const size_t required_frames = 1 + (transfer_bytes - data_avail) / sizeof(struct esco_msbc);
			const size_t padding = pcm_threshold + (required_frames * MSBC_CODESAMPLES) - pcm_avail;
			audio_silence_s16_2le((int16_t *)msbc.pcm.tail, padding , 1, true, false);
			ffb_seek(&msbc.pcm, padding);
		}
	}

exit:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_msbc:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_MSBC
static void *sco_msbc_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct ba_transport_thread *th = t_pcm->th;

	/* SCO transport should deliver audio packets as soon as it is acquired.
	* However, if the adapter does not route incoming SCO to the HCI we will
	* never see those packets. The encoder thread is blocked until this thread
	* signals that the first packet has been received so we set a timeout for
	* the first read only to ensure we can still send that signal. We choose an
	* arbitrary timeout of 100 ms. */
	struct io_poll io = { .timeout = 100 };

	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);

	if (msbc_init(&msbc) != 0) {
		error("Couldn't initialize mSBC codec: %s", strerror(errno));
		goto fail_msbc;
	}

	bool mtu_optimized = false;
	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_thread_state_set_running(th);;) {

		ssize_t len = io_poll_and_read_bt(&io, th, msbc.data.tail, ffb_blen_in(&msbc.data));
		switch (len) {
		case -1: {
			if (errno == ETIME) {
				debug("SCO decoder read timeout");
				io.timeout = -1;
				/* Incoming SCO is not being routed through the HCI. Use socket
				* SCO options MTU. */
				size_t mtu = t->mtu_write;
				if ((t->d->a->hci.type & 0x0F) == HCI_USB) {
					/* The MTU value returned by kernel btusb driver is
					 * incorrect. We use the USB Alt-1 setting of 24 bytes
					 * since this is correct for the majority of currently
					 * available WBS-capable adapters, except for Realtek
					 * adapters which use Alt-3 (72 bytes). */
					mtu = 24;
					struct ba_adapter *a = t->d->a;
					if (a->chip.manufacturer == 0)
						hci_get_version(a->hci.dev_id, &a->chip);
					if (!config.disable_realtek_usb_fix && a->chip.manufacturer == BT_COMPID_REALTEK)
						mtu = 72;
					debug("USB adjusted SCO MTU: %d: %zd", th->bt_fd, mtu);
				}
				else {
					/* Transfer whole mSBC frames if possible. */
					if (mtu >= sizeof(esco_msbc_frame_t))
					mtu = sizeof(esco_msbc_frame_t);
					debug("Adjusted SCO MTU: %d: %zd", th->bt_fd, mtu);
				}
				t->sco.transfer_bytes = (unsigned int) mtu;
				mtu_optimized = true;
				continue;
			}
			error("BT poll and read error: %s", strerror(errno));

			break;
		}
		case 0:
			goto exit;

		default:
			if (!mtu_optimized) {
				t->sco.transfer_bytes = (unsigned int) len;
				io.timeout = -1;
				mtu_optimized = true;
			}
		}
		if (!ba_transport_pcm_is_active(t_pcm))
			continue;

		if (len > 0)
			ffb_seek(&msbc.data, len);

		int err;
again:
		if ((err = msbc_decode(&msbc)) < 0) {
			error("mSBC decoding error: %s", sbc_strerror(err));
			continue;
		}

		ssize_t samples;
		if ((samples = ffb_len_out(&msbc.pcm)) <= 0)
			continue;

		io_pcm_scale(t_pcm, msbc.pcm.data, samples);
		if ((samples = io_pcm_write(t_pcm, msbc.pcm.data, samples)) == -1)
			error("FIFO write error: %s", strerror(errno));
		else if (samples == 0)
			ba_transport_stop_if_no_clients(t);

		ffb_shift(&msbc.pcm, samples);

		/* We must ensure that we can always read a full message from the BT
		 * socket on each call to io_poll_and_read_bt(). Otherwise the unread
		 * fraction left in the controller may be overwritten by the next
		 * incoming SCO packet before we have chance to read it. So if our
		 * decoder buffer has insufficient space we must decode another frame
		 * before continuing. */
		if (ffb_blen_in(&msbc.data) < t->mtu_read)
			goto again;

	}

exit:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_msbc:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

void *sco_enc_thread(struct ba_transport_pcm *pcm) {
	switch (ba_transport_get_codec(pcm->t)) {
	case HFP_CODEC_CVSD:
	default:
		return sco_cvsd_enc_thread(pcm);
#if ENABLE_MSBC
	case HFP_CODEC_MSBC:
		return sco_msbc_enc_thread(pcm);
#endif
	}
}

__attribute__ ((weak))
void *sco_dec_thread(struct ba_transport_pcm *pcm) {
	switch (ba_transport_get_codec(pcm->t)) {
	case HFP_CODEC_CVSD:
	default:
		return sco_cvsd_dec_thread(pcm);
#if ENABLE_MSBC
	case HFP_CODEC_MSBC:
		return sco_msbc_dec_thread(pcm);
#endif
	}
}

void sco_transport_init(struct ba_transport *t) {

	t->sco.pcm_spk.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->sco.pcm_spk.channels = 1;

	t->sco.pcm_mic.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->sco.pcm_mic.channels = 1;

	uint16_t codec_id;
	switch (codec_id = ba_transport_get_codec(t)) {
	case HFP_CODEC_UNDEFINED:
		t->sco.pcm_spk.sampling = 0;
		t->sco.pcm_mic.sampling = 0;
		break;
	case HFP_CODEC_CVSD:
		t->sco.pcm_spk.sampling = 8000;
		t->sco.pcm_mic.sampling = 8000;
		break;
#if ENABLE_MSBC
	case HFP_CODEC_MSBC:
		t->sco.pcm_spk.sampling = 16000;
		t->sco.pcm_mic.sampling = 16000;
		break;
#endif
	default:
		debug("Unsupported SCO codec: %#x", codec_id);
		g_assert_not_reached();
	}

	if (t->sco.pcm_spk.ba_dbus_exported)
		bluealsa_dbus_pcm_update(&t->sco.pcm_spk,
				BA_DBUS_PCM_UPDATE_SAMPLING |
				BA_DBUS_PCM_UPDATE_CODEC |
				BA_DBUS_PCM_UPDATE_DELAY_ADJUSTMENT);

	if (t->sco.pcm_mic.ba_dbus_exported)
		bluealsa_dbus_pcm_update(&t->sco.pcm_mic,
				BA_DBUS_PCM_UPDATE_SAMPLING |
				BA_DBUS_PCM_UPDATE_CODEC |
				BA_DBUS_PCM_UPDATE_DELAY_ADJUSTMENT);

}

int sco_transport_start(struct ba_transport *t) {

	int rv = 0;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_AG) {
		rv |= ba_transport_pcm_start(&t->sco.pcm_spk, sco_enc_thread, "ba-sco-enc", true);
		rv |= ba_transport_pcm_start(&t->sco.pcm_mic, sco_dec_thread, "ba-sco-dec", false);
		return rv;
	}

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_HF) {
		rv |= ba_transport_pcm_start(&t->sco.pcm_spk, sco_dec_thread, "ba-sco-dec", true);
		rv |= ba_transport_pcm_start(&t->sco.pcm_mic, sco_enc_thread, "ba-sco-enc", false);
		return rv;
	}

	g_assert_not_reached();
	return -1;
}
