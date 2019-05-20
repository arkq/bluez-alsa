/*
 * BlueALSA - bluealsa.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEALSA_H_
#define BLUEALSA_BLUEALSA_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <glib.h>

struct ba_config {

	/* set of enabled profiles */
	struct {
		bool a2dp_source;
		bool a2dp_sink;
		bool hfp_ofono;
		bool hfp_hf;
		bool hfp_ag;
		bool hsp_hs;
		bool hsp_ag;
	} enable;

	/* established D-Bus connection */
	GDBusConnection *dbus;

	/* adapters indexed by the HCI device ID */
	struct ba_adapter *adapters[HCI_MAX_DEV];

	/* List of HCI names (or BT addresses) used for adapters filtering
	 * during profile registration. Leave it empty to use any adapter. */
	GArray *hci_filter;

	/* used for main thread identification */
	pthread_t main_thread;

	/* opened null device */
	int null_fd;

	struct {
		/* set of features exposed via Service Discovery */
		int features_sdp_hf;
		int features_sdp_ag;
		/* set of features exposed via RFCOMM connection */
		int features_rfcomm_hf;
		int features_rfcomm_ag;
	} hfp;

	struct {

		/* NULL-terminated list of available A2DP codecs */
		const struct bluez_a2dp_codec **codecs;

		/* Control audio volume natively by the connected device. The disadvantage
		 * of this control type is a monophonic volume change. */
		bool volume;

		/* Support for monophonic sound in the A2DP profile is mandatory for
		 * sink and semi-mandatory for source. So, if one wants only the bare
		 * minimum, it would be possible - e.g. due to bandwidth limitations. */
		bool force_mono;
		/* The sampling rates of 44.1 kHz (aka Audio CD) and 48 kHz are mandatory
		 * for sink endpoint and semi-mandatory for source. It is then possible
		 * to force lower sampling in order to save Bluetooth bandwidth. */
		bool force_44100;

		/* The number of seconds for keeping A2DP transport alive after PCM has
		 * been closed. One might set this value to negative number for infinite
		 * time. This option applies for the source profile only. */
		int keep_alive;

	} a2dp;

#if ENABLE_AAC
	bool aac_afterburner;
	uint8_t aac_vbr_mode;
#endif

#if ENABLE_LDAC
	bool ldac_abr;
	uint8_t ldac_eqmid;
#endif

};

/* Global BlueALSA configuration. */
extern struct ba_config config;

int bluealsa_config_init(void);

#endif
