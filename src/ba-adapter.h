/*
 * BlueALSA - ba-adapter.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BAADAPTER_H_
#define BLUEALSA_BAADAPTER_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>

#include <glib.h>

/* Data associated with BT adapter. */
struct ba_adapter {

	int hci_dev_id;
	char hci_name[8];

	/* data for D-Bus management */
	char ba_dbus_path[32];
	char bluez_dbus_path[32];

	/* collection of connected devices */
	pthread_mutex_t devices_mutex;
	GHashTable *devices;

	/* associated controller */
	struct ba_ctl *ctl;

};

struct ba_adapter *ba_adapter_new(int dev_id, const char *name);
struct ba_adapter *ba_adapter_lookup(int dev_id);
void ba_adapter_free(struct ba_adapter *a);

#endif
