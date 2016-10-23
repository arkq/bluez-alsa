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

#include <poll.h>
#include <pthread.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <glib.h>
#include <gio/gio.h>

/* Maximal number of clients connected to the controller. */
#define BLUEALSA_MAX_CLIENTS 7

struct ba_setup {

	/* used HCI device */
	struct hci_dev_info hci_dev;

	gboolean enable_a2dp;
	gboolean enable_hsp;

	/* established D-Bus connection */
	GDBusConnection *dbus;

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

};

int bluealsa_setup_init(struct ba_setup *setup);
void bluealsa_setup_free(struct ba_setup *setup);

#endif
