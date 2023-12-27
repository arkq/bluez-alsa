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

#include "ba-device.h"
#include "ba-transport-pcm.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#if ENABLE_MSBC
# include "codec-msbc.h"
#endif
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
		t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd, a);
		fd = -1;

		pthread_mutex_unlock(&t->bt_fd_mtx);

		ba_transport_pcm_state_set_idle(&t->sco.pcm_spk);
		ba_transport_pcm_state_set_idle(&t->sco.pcm_mic);
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

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t samples = ffb_len_in(&buffer);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, buffer.tail, samples)) {
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
		samples = ffb_len_out(&buffer);

		const int16_t *input = buffer.data;
		size_t input_samples = samples;

		while (input_samples >= mtu_samples) {

			ssize_t ret;
			if ((ret = io_bt_write(t_pcm, input, mtu_write)) <= 0) {
				if (ret == -1)
					error("BT write error: %s", strerror(errno));
				goto exit;
			}

			input += mtu_samples;
			input_samples -= mtu_samples;

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, mtu_samples);
			/* update busy delay (encoding overhead) */
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

		}

		ffb_shift(&buffer, samples - input_samples);

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
	struct io_poll io = { .timeout = -1 };

	const size_t mtu_samples = t->mtu_read / sizeof(int16_t);
	const size_t mtu_samples_multiplier = 2;

	ffb_t buffer = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

	if (ffb_init_int16_t(&buffer, mtu_samples * mtu_samples_multiplier) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len = ffb_blen_in(&buffer);
		if ((len = io_poll_and_read_bt(&io, t_pcm, buffer.tail, len)) == -1)
			error("BT poll and read error: %s", strerror(errno));
		else if (len == 0)
			goto exit;

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
static void *sco_msbc_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };
	const size_t mtu_write = t->mtu_write;

	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);

	if ((errno = -msbc_init(&msbc)) != 0) {
		error("Couldn't initialize mSBC codec: %s", strerror(errno));
		goto fail_msbc;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t samples = ffb_len_in(&msbc.pcm);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, msbc.pcm.tail, samples)) {
		case -1:
			if (errno == ESTALE) {
				/* reinitialize mSBC encoder */
				msbc_init(&msbc);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		ffb_seek(&msbc.pcm, samples);

		while (ffb_len_out(&msbc.pcm) >= MSBC_CODESAMPLES) {

			int err;
			if ((err = msbc_encode(&msbc)) < 0) {
				error("mSBC encoding error: %s", msbc_strerror(err));
				break;
			}

			uint8_t *data = msbc.data.data;
			size_t data_len = ffb_blen_out(&msbc.data);

			while (data_len >= mtu_write) {

				ssize_t len;
				if ((len = io_bt_write(t_pcm, data, mtu_write)) <= 0) {
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
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

			/* Move unprocessed data to the front of our linear
			 * buffer and clear the mSBC frame counter. */
			ffb_shift(&msbc.data, ffb_blen_out(&msbc.data) - data_len);
			msbc.frames = 0;

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
	struct io_poll io = { .timeout = -1 };

	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);

	if ((errno = -msbc_init(&msbc)) != 0) {
		error("Couldn't initialize mSBC codec: %s", strerror(errno));
		goto fail_msbc;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len = ffb_blen_in(&msbc.data);
		if ((len = io_poll_and_read_bt(&io, t_pcm, msbc.data.tail, len)) == -1)
			error("BT poll and read error: %s", strerror(errno));
		else if (len == 0)
			goto exit;

		if (!ba_transport_pcm_is_active(t_pcm))
			continue;

		if (len > 0)
			ffb_seek(&msbc.data, len);

		int err;
		/* Process data until there is no more mSBC frames to decode. This loop
		 * ensures that for MTU values bigger than the mSBC frame size, the input
		 * buffer will not fill up causing short reads and mSBC frame losses. */
		while ((err = msbc_decode(&msbc)) > 0)
			continue;
		if (err < 0) {
			error("mSBC decoding error: %s", msbc_strerror(err));
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
		rv |= ba_transport_pcm_start(&t->sco.pcm_spk, sco_enc_thread, "ba-sco-enc");
		rv |= ba_transport_pcm_start(&t->sco.pcm_mic, sco_dec_thread, "ba-sco-dec");
		return rv;
	}

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_HF) {
		rv |= ba_transport_pcm_start(&t->sco.pcm_spk, sco_dec_thread, "ba-sco-dec");
		rv |= ba_transport_pcm_start(&t->sco.pcm_mic, sco_enc_thread, "ba-sco-enc");
		return rv;
	}

	g_assert_not_reached();
	return -1;
}
