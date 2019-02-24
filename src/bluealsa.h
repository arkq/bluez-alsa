/*
 * BlueALSA - bluealsa.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEALSA_H
#define BLUEALSA_BLUEALSA_H

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdbool.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <glib.h>
#include <gio/gio.h>

#include "bluez.h"
#include "bluez-a2dp.h"
#include "ctl.h"
#include "transport.h"
#include "shared/ctl-proto.h"

struct ba_config {

	/* used HCI device */
	struct hci_dev_info hci_dev;

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

	/* used for main thread identification */
	pthread_t main_thread;

	/* collection of connected devices */
	pthread_mutex_t devices_mutex;
	GHashTable *devices;

	/* registered D-Bus objects */
	GHashTable *dbus_objects;

	/* opened null device */
	int null_fd;

	/* audio group ID */
	gid_t gid_audio;

	/* global controller */
	struct ba_ctl *ctl;

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

/* Structure describing registered D-Bus object. */
struct ba_dbus_object {
	/* D-Bus object registration ID */
	unsigned int id;
	struct ba_transport_type ttype;
	/* determine whether profile is used */
	bool connected;
};

/* Global BlueALSA configuration. */
extern struct ba_config config;

int bluealsa_config_init(void);

#define bluealsa_devpool_mutex_lock() \
	pthread_mutex_lock(&config.devices_mutex)
#define bluealsa_devpool_mutex_unlock() \
	pthread_mutex_unlock(&config.devices_mutex)

#define bluealsa_device_insert(key, device) \
	g_hash_table_insert(config.devices, g_strdup(key), device)
#define bluealsa_device_lookup(key) \
	((struct ba_device *)g_hash_table_lookup(config.devices, key))
#define bluealsa_device_remove(key) \
	g_hash_table_remove(config.devices, key)

#endif
