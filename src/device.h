/*
 * BlueALSA - device.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_DEVICE_H_
#define BLUEALSA_DEVICE_H_

#include <bluetooth/bluetooth.h>
#include <glib.h>

#include "transport.h"

struct ba_device {

	bdaddr_t addr;
	char *name;

	/* collection of connected transports */
	GHashTable *transports;

};

struct ba_device *device_new(bdaddr_t *addr, const char *name);
void device_free(struct ba_device *d);

struct ba_transport *device_transport_lookup(GHashTable *devices, const char *key);
gboolean device_transport_remove(GHashTable *devices, const char *key);

/* Helper macro for proper initialization of the device list structure. */
#define devices_init() \
	g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)device_free)

#endif
