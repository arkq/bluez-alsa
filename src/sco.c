/*
 * BlueALSA - sco.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "sco.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sco.h>

#include "ba-device.h"
#include "bluealsa.h"
#include "codec-msbc.h"
#include "hci.h"
#include "hfp.h"
#include "io.h"
#include "utils.h"
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

		if (poll(&data.pfd, 1, -1) == -1) {
			if (errno == EINTR)
				continue;
			error("SCO dispatcher poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		struct sockaddr_sco addr;
		socklen_t addrlen = sizeof(addr);
		struct ba_device *d = NULL;
		struct ba_transport *t = NULL;
		int fd = -1;

		if ((fd = accept(data.pfd.fd, (struct sockaddr *)&addr, &addrlen)) == -1) {
			error("Couldn't accept incoming SCO link: %s", strerror(errno));
			goto cleanup;
		}

		debug("New incoming SCO link: %s: %d", batostr_(&addr.sco_bdaddr), fd);

		if ((d = ba_device_lookup(data.a, &addr.sco_bdaddr)) == NULL) {
			error("Couldn't lookup device: %s", batostr_(&addr.sco_bdaddr));
			goto cleanup;
		}

		if ((t = ba_transport_lookup(d, d->bluez_dbus_path)) == NULL) {
			error("Couldn't lookup transport: %s", d->bluez_dbus_path);
			goto cleanup;
		}

#if ENABLE_MSBC
		struct bt_voice voice = { .setting = BT_VOICE_TRANSPARENT };
		if (t->type.codec == HFP_CODEC_MSBC &&
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
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
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
					PTHREAD_ROUTINE(sco_dispatcher_thread), a)) != 0) {
		error("Couldn't create SCO dispatcher: %s", strerror(ret));
		a->sco_dispatcher = config.main_thread;
		return -1;
	}

	pthread_setname_np(a->sco_dispatcher, "ba-sco-dispatch");
	debug("Created SCO dispatcher [%s]: %s", "ba-sco-dispatch", a->hci.name);

	return 0;
}

static void *sco_cvsd_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	/* buffers for transferring data to and from SCO socket */
	ffb_t bt_in = { 0 };
	ffb_t bt_out = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt_in);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt_out);

	/* these buffers shall be bigger than the SCO MTU */
	if (ffb_init_uint8_t(&bt_in, 128) == -1 ||
			ffb_init_uint8_t(&bt_out, 128) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_ffb;
	}

	int poll_timeout = -1;
	struct ba_transport *t = th->t;
	struct asrsync asrs = { .frames = 0 };
	struct pollfd pfds[] = {
		{ th->pipe[0], POLLIN, 0 },
		/* SCO socket */
		{ -1, POLLIN, 0 },
		{ -1, POLLOUT, 0 },
		/* PCM FIFO */
		{ -1, POLLIN, 0 },
		{ -1, POLLOUT, 0 },
	};

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		/* fresh-start for file descriptors polling */
		pfds[1].fd = pfds[2].fd = -1;
		pfds[3].fd = pfds[4].fd = -1;

			if (ffb_len_in(&bt_in) >= t->mtu_read)
				pfds[1].fd = th->bt_fd;
			if (ffb_len_out(&bt_out) >= t->mtu_write)
				pfds[2].fd = th->bt_fd;
			if (t->sco.spk_pcm.active && th->bt_fd != -1 && ffb_len_in(&bt_out) >= t->mtu_write)
				pfds[3].fd = t->sco.spk_pcm.fd;
			if (t->sco.mic_pcm.active && ffb_len_out(&bt_in) > 0)
				pfds[4].fd = t->sco.mic_pcm.fd;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		switch (poll(pfds, ARRAYSIZE(pfds), poll_timeout)) {
		case 0:
			pthread_cond_signal(&t->sco.spk_pcm.synced);
			poll_timeout = -1;
			continue;
		case -1:
			if (errno == EINTR)
				continue;
			error("SCO poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			enum ba_transport_thread_signal signal;
			ba_transport_thread_signal_recv(th, &signal);
			switch (signal) {
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_OPEN:
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_RESUME:
				asrs.frames = 0;
				continue;
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_CLOSE:
				ba_transport_stop_if_no_clients(t);
				continue;
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_SYNC:
				/* FIXME: Drain functionality for speaker.
				 * XXX: Right now it is not possible to drain speaker PCM (in a clean
				 *      fashion), because poll() will not timeout if we've got incoming
				 *      data from the microphone (BT SCO socket). In order not to hang
				 *      forever in the transport_drain_pcm() function, we will signal
				 *      PCM drain right now. */
				pthread_cond_signal(&t->sco.spk_pcm.synced);
				break;
			case BA_TRANSPORT_THREAD_SIGNAL_PCM_DROP:
				io_pcm_flush(&t->sco.spk_pcm);
				continue;
			default:
				break;
			}
		}

		if (asrs.frames == 0)
			asrsync_init(&asrs, t->sco.spk_pcm.sampling);

		if (pfds[1].revents & (POLLIN | POLLHUP)) {
			/* dispatch incoming SCO data */

			uint8_t *buffer;
			size_t buffer_len;

				if (t->sco.mic_pcm.fd == -1)
					ffb_rewind(&bt_in);
				buffer = bt_in.tail;
				buffer_len = ffb_blen_in(&bt_in);

			ssize_t len;
			if ((len = io_bt_read(th, buffer, buffer_len)) <= 0) {
				if (len == -1)
					debug("BT read error: %s", strerror(errno));
				goto fail;
			}

			/* If microphone (capture) PCM is not connected ignore incoming data. In
			 * the worst case scenario, we might lose few milliseconds of data (one
			 * mSBC frame which is 7.5 ms), but we will be sure, that the microphone
			 * latency will not build up. */
			if (t->sco.mic_pcm.fd != -1)
					ffb_seek(&bt_in, len);

		}
		else if (pfds[1].revents) {
			error("SCO poll error: %#x", pfds[1].revents);
		}

		if (pfds[2].revents & POLLOUT) {
			/* write-out SCO data */

			uint8_t *buffer;
			size_t buffer_len;

				buffer = bt_out.data;
				buffer_len = t->mtu_write;

			ssize_t len;
			if ((len = io_bt_write(th, buffer, buffer_len)) <= 0) {
				if (len == -1)
					error("SCO write error: %s", strerror(errno));
				goto fail;
			}

				ffb_shift(&bt_out, len);

		}

		if (pfds[3].revents & (POLLIN | POLLHUP)) {
			/* dispatch incoming PCM data */

			int16_t *buffer;
			ssize_t samples;

				buffer = (int16_t *)bt_out.tail;
				samples = ffb_len_in(&bt_out) / sizeof(int16_t);

			if ((samples = io_pcm_read(&t->sco.spk_pcm, buffer, samples)) <= 0) {
				if (samples == -1 && errno != EAGAIN)
					error("PCM read error: %s", strerror(errno));
				if (samples == 0)
					ba_transport_thread_signal_send(th, BA_TRANSPORT_THREAD_SIGNAL_PCM_CLOSE);
				continue;
			}

				ffb_seek(&bt_out, samples * sizeof(int16_t));

		}
		else if (pfds[3].revents) {
			error("PCM poll error: %#x", pfds[3].revents);
		}

		if (pfds[4].revents & POLLOUT) {
			/* write-out PCM data */

			int16_t *buffer;
			ssize_t samples;

				buffer = (int16_t *)bt_in.data;
				samples = ffb_len_out(&bt_in) / sizeof(int16_t);

			io_pcm_scale(&t->sco.mic_pcm, buffer, samples);
			if ((samples = io_pcm_write(&t->sco.mic_pcm, buffer, samples)) <= 0) {
				if (samples == -1)
					error("FIFO write error: %s", strerror(errno));
				if (samples == 0)
					ba_transport_thread_signal_send(th, BA_TRANSPORT_THREAD_SIGNAL_PCM_CLOSE);
			}

				ffb_shift(&bt_in, samples * sizeof(int16_t));

		}

		/* keep data transfer at a constant bit rate */
			asrsync_sync(&asrs, t->mtu_write / sizeof(int16_t));

		/* update busy delay (encoding overhead) */
		const unsigned int delay = asrsync_get_busy_usec(&asrs) / 100;
		t->sco.spk_pcm.delay = t->sco.mic_pcm.delay = delay;

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static void *sco_cvsd_enc_thread(struct ba_transport_thread *th) {
	return sco_cvsd_thread(th);
}

static void *sco_cvsd_dec_thread(struct ba_transport_thread *th) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	for (ba_transport_thread_set_state_running(th);;) {
		enum ba_transport_thread_signal signal;
		ba_transport_thread_signal_recv(th, &signal);
		ba_transport_thread_signal_send(&th->t->thread_enc, signal);
	}

	pthread_cleanup_pop(1);
	return NULL;
}

#if ENABLE_MSBC
static void *sco_msbc_enc_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct ba_transport_pcm *pcm = &t->sco.spk_pcm;
	struct io_poll io = { .timeout = -1 };
	const size_t mtu_write = t->mtu_write;

	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);

	if (msbc_init(&msbc) != 0) {
		error("Couldn't initialize mSBC codec: %s", strerror(errno));
		goto fail_msbc;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples = ffb_len_in(&msbc.pcm);
		if ((samples = io_poll_and_read_pcm(&io, pcm, msbc.pcm.tail, samples)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			else if (samples == 0)
				ba_transport_stop_if_no_clients(t);
			continue;
		}

		ffb_seek(&msbc.pcm, samples);
		if (msbc_encode(&msbc) == -1) {
			warn("Couldn't encode mSBC: %s", strerror(errno));
			ffb_rewind(&msbc.pcm);
		}

		if (msbc.frames == 0)
			continue;

		uint8_t *data = msbc.data.data;
		size_t data_len = ffb_blen_out(&msbc.data);

		while (data_len >= mtu_write) {

			ssize_t len;
			if ((len = io_bt_write(th, data, mtu_write)) <= 0) {
				if (len == -1)
					error("BT write error: %s", strerror(errno));
				goto exit;
			}

			data += len;
			data_len -= len;

		}

		/* keep data transfer at a constant bit rate */
		asrsync_sync(&io.asrs, msbc.frames * MSBC_CODESAMPLES);
		/* update busy delay (encoding overhead) */
		pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

		/* Move unprocessed data to the front of our linear
		 * buffer and clear the mSBC frame counter. */
		ffb_shift(&msbc.data, ffb_blen_out(&msbc.data) - data_len);
		msbc.frames = 0;

	}

exit:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_msbc:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_MSBC
static void *sco_msbc_dec_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct ba_transport_pcm *pcm = &t->sco.mic_pcm;
	struct io_poll io = { .timeout = -1 };

	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);

	if (msbc_init(&msbc) != 0) {
		error("Couldn't initialize mSBC codec: %s", strerror(errno));
		goto fail_msbc;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len = ffb_blen_in(&msbc.data);
		if ((len = io_poll_and_read_bt(&io, th, msbc.data.tail, len)) == -1)
			error("BT poll and read error: %s", strerror(errno));
		else if (len == 0)
			goto exit;

		if (!ba_transport_pcm_is_active(pcm))
			continue;

		ffb_seek(&msbc.data, len);
		if (msbc_decode(&msbc) == -1) {
			warn("Couldn't decode mSBC: %s", strerror(errno));
			ffb_rewind(&msbc.data);
		}

		ssize_t samples;
		if ((samples = ffb_len_out(&msbc.pcm)) <= 0)
			continue;

		io_pcm_scale(pcm, msbc.pcm.data, samples);
		if ((samples = io_pcm_write(pcm, msbc.pcm.data, samples)) == -1)
			error("FIFO write error: %s", strerror(errno));
		else if (samples == 0)
			ba_transport_stop_if_no_clients(t);

		ffb_shift(&msbc.pcm, samples);

	}

exit:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_msbc:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

void *sco_enc_thread(struct ba_transport_thread *th) {
	switch (th->t->type.codec) {
	case HFP_CODEC_CVSD:
	default:
		return sco_cvsd_enc_thread(th);
#if ENABLE_MSBC
	case HFP_CODEC_MSBC:
		return sco_msbc_enc_thread(th);
#endif
	}
}

void *sco_dec_thread(struct ba_transport_thread *th) {
	switch (th->t->type.codec) {
	case HFP_CODEC_CVSD:
	default:
		return sco_cvsd_dec_thread(th);
#if ENABLE_MSBC
	case HFP_CODEC_MSBC:
		return sco_msbc_dec_thread(th);
#endif
	}
}
