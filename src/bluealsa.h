/*
 * BlueALSA - bluealsa.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEALSA_H
#define BLUEALSA_BLUEALSA_H

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <poll.h>
#include <pthread.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <glib.h>
#include <gio/gio.h>

/* Maximal number of clients connected to the controller. */
#define BLUEALSA_MAX_CLIENTS 7

struct ba_config {

	/* used HCI device */
	struct hci_dev_info hci_dev;

	gboolean enable_a2dp;
	gboolean enable_hsp;
	gboolean enable_hfp;

	/* established D-Bus connection */
	GDBusConnection *dbus;

	/* used for main thread identification */
	pthread_t main_thread;

	/* collection of connected devices */
	pthread_mutex_t devices_mutex;
	GHashTable *devices;

	/* registered D-Bus objects */
	GHashTable *dbus_objects;

	/* audio group ID */
	gid_t gid_audio;

	pthread_t ctl_thread;
	struct pollfd ctl_pfds[1 + BLUEALSA_MAX_CLIENTS];

	gboolean ctl_socket_created;
	gboolean ctl_thread_created;

#if ENABLE_AAC
	gboolean aac_afterburner;
	uint8_t aac_vbr_mode;
#endif

	/* Support for monophonic sound in the A2DP profile is mandatory for
	 * sink and semi-mandatory for source. So, if one wants only the bare
	 * minimum, it would be possible - e.g. due to bandwidth limitations. */
	gboolean a2dp_force_mono;
	/* The sampling rates of 44.1 kHz (aka Audio CD) and 48 kHz are mandatory
	 * for sink endpoint and semi-mandatory for source. It is then possible
	 * to force lower sampling in order to save Bluetooth bandwidth. */
	gboolean a2dp_force_44100;
	/* Control audio volume natively by the connected device. The disadvantage
	 * of this control type is a monophonic volume change. */
	gboolean a2dp_volume;

};

/* Global BlueALSA configuration. */
extern struct ba_config config;

int bluealsa_config_init(void);
void bluealsa_config_free(void);

#endif
