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
#include "bluealsa-config.h"
#if ENABLE_MSBC
# include "codec-msbc.h"
#endif
#include "codec-sbc.h"
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
		t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd, a->hci.type);
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

static void *sco_cvsd_enc_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct ba_transport_pcm *pcm = &t->sco.spk_pcm;
	struct io_poll io = { .timeout = -1 };

	const size_t mtu_samples = t->mtu_write / sizeof(int16_t);
	const size_t mtu_write = t->mtu_write;

	ffb_t buffer = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

	/* define a bigger buffer to enhance read performance */
	if (ffb_init_int16_t(&buffer, mtu_samples * 4) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_init;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples = ffb_len_in(&buffer);
		if ((samples = io_poll_and_read_pcm(&io, pcm, buffer.tail, samples)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			else if (samples == 0)
				ba_transport_stop_if_no_clients(t);
			continue;
		}

		ffb_seek(&buffer, samples);
		samples = ffb_len_out(&buffer);

		const int16_t *input = buffer.data;
		size_t input_samples = samples;

		while (input_samples >= mtu_samples) {

			ssize_t ret;
			if ((ret = io_bt_write(th, input, mtu_write)) <= 0) {
				if (ret == -1)
					error("BT write error: %s", strerror(errno));
				goto exit;
			}

			input += mtu_samples;
			input_samples -= mtu_samples;

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, mtu_samples);
			/* update busy delay (encoding overhead) */
			pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

		}

		ffb_shift(&buffer, samples - input_samples);

	}

exit:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
fail_init:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static void *sco_cvsd_dec_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct ba_transport_pcm *pcm = &t->sco.mic_pcm;
	struct io_poll io = { .timeout = -1 };

	const size_t mtu_samples = t->mtu_read / sizeof(int16_t);
	const size_t mtu_samples_multiplier = 2;

	ffb_t buffer = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

	if (ffb_init_int16_t(&buffer, mtu_samples * mtu_samples_multiplier) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len = ffb_blen_in(&buffer);
		if ((len = io_poll_and_read_bt(&io, th, buffer.tail, len)) == -1)
			error("BT poll and read error: %s", strerror(errno));
		else if (len == 0)
			goto exit;

		if ((size_t)len == buffer.nmemb * buffer.size) {
			debug("Resizing CVSD read buffer: %zd -> %zd",
					buffer.nmemb * buffer.size, buffer.nmemb * buffer.size * 2);
			if (ffb_init_int16_t(&buffer, buffer.nmemb * 2) == -1)
				error("Couldn't resize CVSD read buffer: %s", strerror(errno));
		}

		if (!ba_transport_pcm_is_active(pcm))
			continue;

		ssize_t samples = len / sizeof(int16_t);
		io_pcm_scale(pcm, buffer.data, samples);
		if ((samples = io_pcm_write(pcm, buffer.data, samples)) == -1)
			error("FIFO write error: %s", strerror(errno));
		else if (samples == 0)
			ba_transport_stop_if_no_clients(t);

	}

exit:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
fail_ffb:
	pthread_cleanup_pop(1);
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

		while (ffb_len_out(&msbc.pcm) >= MSBC_CODESAMPLES) {

			int err;
			if ((err = msbc_encode(&msbc)) < 0) {
				error("mSBC encoding error: %s", sbc_strerror(err));
				break;
			}

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

	}

exit:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
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

		int err;
		if ((err = msbc_decode(&msbc)) < 0) {
			error("mSBC decoding error: %s", sbc_strerror(err));
			continue;
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
