/*
 * BlueALSA - ba-device.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ba-device.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ba-transport.h"
#include "bluealsa.h"
#include "bluez-iface.h"
#include "utils.h"

struct ba_device *ba_device_new(
		struct ba_adapter *adapter,
		const bdaddr_t *addr) {

#if DEBUG
	/* make sure that the device mutex is acquired */
	g_assert(pthread_mutex_trylock(&adapter->devices_mutex) == EBUSY);
#endif

	struct ba_device *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return NULL;

	d->a = adapter;
	bacpy(&d->addr, addr);

	char tmp[sizeof("dev_XX:XX:XX:XX:XX:XX")];
	sprintf(tmp, "dev_%.2X_%.2X_%.2X_%.2X_%.2X_%.2X",
			addr->b[5], addr->b[4], addr->b[3], addr->b[2], addr->b[1], addr->b[0]);
	d->ba_dbus_path = g_strdup_printf("%s/%s", adapter->ba_dbus_path, tmp);
	d->bluez_dbus_path = g_strdup_printf("%s/%s", adapter->bluez_dbus_path, tmp);

	d->battery_level = -1;

	d->transports = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);

	g_hash_table_insert(adapter->devices, &d->addr, d);
	return d;
}

struct ba_device *ba_device_lookup(
		struct ba_adapter *adapter,
		const bdaddr_t *addr) {
#if DEBUG
	/* make sure that the device mutex is acquired */
	g_assert(pthread_mutex_trylock(&adapter->devices_mutex) == EBUSY);
#endif
	return g_hash_table_lookup(adapter->devices, addr);
}

void ba_device_free(struct ba_device *d) {

	if (d == NULL)
		return;

	/* detach device from the adapter */
	g_hash_table_steal(d->a->devices, &d->addr);

	/* XXX: Modification-safe remove-all loop.
	 *
	 * By the usage of a standard g_hash_table_remove_all() function, one
	 * has to comply to the license warranty, which states that anything
	 * can happen. In our case it is true to the letter - SIGSEGV is 100%
	 * guaranteed.
	 *
	 * Our transport structure holds reference to some other transport
	 * structure within the same hash-table. Unfortunately, such a usage
	 * is not supported. Almost every GLib-2.0 function facilitates cache,
	 * which backfires at us if we modify hash-table from the inside of
	 * the destroy function. However, it is possible to "iterate" over
	 * a hash-table in a pop-like matter - reinitialize iterator after
	 * every modification. And voila - modification-safe remove loop. */
	for (;;) {

		GHashTableIter iter;
		struct ba_transport *t;

		g_hash_table_iter_init(&iter, d->transports);
		if (!g_hash_table_iter_next(&iter, NULL, (gpointer)&t))
			break;

		ba_transport_free(t);
	}

	g_hash_table_unref(d->transports);
	g_free(d->bluez_dbus_path);
	g_free(d->ba_dbus_path);
	free(d);
}
