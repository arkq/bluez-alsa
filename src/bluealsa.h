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
#include <stdbool.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <glib.h>
#include <gio/gio.h>

#include "bluez.h"

/* Maximal number of clients connected to the controller. */
#define BLUEALSA_MAX_CLIENTS 7

/* Indexes of special file descriptors in the poll array. */
#define CTL_IDX_SRV 0
#define CTL_IDX_EVT 1
#define __CTL_IDX_MAX 2

struct ba_config {

	/* used HCI device */
	struct hci_dev_info hci_dev;

	bool enable_a2dp;
	bool enable_hsp;
	bool enable_hfp;

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

	struct {

		pthread_t thread;
		bool socket_created;
		bool thread_created;

		struct pollfd pfds[__CTL_IDX_MAX + BLUEALSA_MAX_CLIENTS];
		/* clients subscribed for notifications */
		bool subs[BLUEALSA_MAX_CLIENTS];

	} ctl;

#if ENABLE_AAC
	bool aac_afterburner;
	uint8_t aac_vbr_mode;
#endif

	/* Support for monophonic sound in the A2DP profile is mandatory for
	 * sink and semi-mandatory for source. So, if one wants only the bare
	 * minimum, it would be possible - e.g. due to bandwidth limitations. */
	bool a2dp_force_mono;
	/* The sampling rates of 44.1 kHz (aka Audio CD) and 48 kHz are mandatory
	 * for sink endpoint and semi-mandatory for source. It is then possible
	 * to force lower sampling in order to save Bluetooth bandwidth. */
	bool a2dp_force_44100;
	/* Control audio volume natively by the connected device. The disadvantage
	 * of this control type is a monophonic volume change. */
	bool a2dp_volume;

};

/* Structure describing registered D-Bus object. */
struct ba_dbus_object {
	/* D-Bus object registration ID */
	unsigned int id;
	enum bluetooth_profile profile;
	uint16_t codec;
	/* determine whether profile is used */
	bool connected;
};

/* Global BlueALSA configuration. */
extern struct ba_config config;

int bluealsa_config_init(void);
void bluealsa_config_free(void);

void bluealsa_event();

#endif
