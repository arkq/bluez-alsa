/*
 * BlueALSA - sco.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sco.h>

#include <glib.h>

#include "ba-config.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "hci.h"
#include "hfp.h"
#include "sco-cvsd.h"
#include "sco-lc3-swb.h"
#include "sco-msbc.h"
#include "shared/bluetooth.h"
#include "shared/defs.h"
#include "shared/log.h"

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

#if ENABLE_HFP_CODEC_SELECTION
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

#if ENABLE_HFP_CODEC_SELECTION
		const uint32_t codec_id = ba_transport_get_codec(t);
		struct bt_voice voice = { .setting = BT_VOICE_TRANSPARENT };
		if ((codec_id == HFP_CODEC_MSBC || codec_id == HFP_CODEC_LC3_SWB) &&
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

void *sco_enc_thread(struct ba_transport_pcm *pcm) {
	switch (ba_transport_get_codec(pcm->t)) {
	case HFP_CODEC_CVSD:
	default:
		return sco_cvsd_enc_thread(pcm);
#if ENABLE_MSBC
	case HFP_CODEC_MSBC:
		return sco_msbc_enc_thread(pcm);
#endif
#if ENABLE_LC3_SWB
	case HFP_CODEC_LC3_SWB:
		return sco_lc3_swb_enc_thread(pcm);
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
#if ENABLE_LC3_SWB
	case HFP_CODEC_LC3_SWB:
		return sco_lc3_swb_dec_thread(pcm);
#endif
	}
}

int sco_transport_init(struct ba_transport *t) {

	t->sco.pcm_spk.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->sco.pcm_spk.channels = 1;
	t->sco.pcm_spk.channel_map[0] = BA_TRANSPORT_PCM_CHANNEL_MONO;

	t->sco.pcm_mic.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->sco.pcm_mic.channels = 1;
	t->sco.pcm_mic.channel_map[0] = BA_TRANSPORT_PCM_CHANNEL_MONO;

	uint32_t codec_id;
	switch (codec_id = ba_transport_get_codec(t)) {
	case HFP_CODEC_UNDEFINED:
		t->sco.pcm_spk.rate = 0;
		t->sco.pcm_mic.rate = 0;
		break;
	case HFP_CODEC_CVSD:
		t->sco.pcm_spk.rate = 8000;
		t->sco.pcm_mic.rate = 8000;
		break;
#if ENABLE_MSBC
	case HFP_CODEC_MSBC:
		t->sco.pcm_spk.rate = 16000;
		t->sco.pcm_mic.rate = 16000;
		break;
#endif
#if ENABLE_LC3_SWB
	case HFP_CODEC_LC3_SWB:
		t->sco.pcm_spk.rate = 32000;
		t->sco.pcm_mic.rate = 32000;
		break;
#endif
	default:
		debug("Unsupported SCO codec: %#x", codec_id);
		g_assert_not_reached();
	}

	if (t->sco.pcm_spk.ba_dbus_exported)
		bluealsa_dbus_pcm_update(&t->sco.pcm_spk,
				BA_DBUS_PCM_UPDATE_RATE |
				BA_DBUS_PCM_UPDATE_CODEC |
				BA_DBUS_PCM_UPDATE_CLIENT_DELAY);

	if (t->sco.pcm_mic.ba_dbus_exported)
		bluealsa_dbus_pcm_update(&t->sco.pcm_mic,
				BA_DBUS_PCM_UPDATE_RATE |
				BA_DBUS_PCM_UPDATE_CODEC |
				BA_DBUS_PCM_UPDATE_CLIENT_DELAY);

	return 0;
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
