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

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "ba-transport.h"
#include "bluealsa.h"
#include "hci.h"
#include "shared/log.h"

struct ba_device *ba_device_new(
		struct ba_adapter *adapter,
		const bdaddr_t *addr) {

	struct ba_device *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return NULL;

	d->a = ba_adapter_ref(adapter);
	bacpy(&d->addr, addr);
	d->ref_count = 1;

	d->seq = atomic_fetch_add_explicit(&config.device_seq, 1, memory_order_relaxed);

	char tmp[sizeof("dev_XX:XX:XX:XX:XX:XX")];
	sprintf(tmp, "dev_%.2X_%.2X_%.2X_%.2X_%.2X_%.2X",
			addr->b[5], addr->b[4], addr->b[3], addr->b[2], addr->b[1], addr->b[0]);
	d->ba_dbus_path = g_strdup_printf("%s/%s", adapter->ba_dbus_path, tmp);
	d->bluez_dbus_path = g_strdup_printf("%s/%s", adapter->bluez_dbus_path, tmp);

	d->battery_level = -1;

	pthread_mutex_init(&d->transports_mutex, NULL);
	d->transports = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);

	pthread_mutex_lock(&adapter->devices_mutex);
	g_hash_table_insert(adapter->devices, &d->addr, d);
	pthread_mutex_unlock(&adapter->devices_mutex);

	return d;
}

struct ba_device *ba_device_lookup(
		struct ba_adapter *adapter,
		const bdaddr_t *addr) {

	struct ba_device *d;

	pthread_mutex_lock(&adapter->devices_mutex);
	if ((d = g_hash_table_lookup(adapter->devices, addr)) != NULL)
		d->ref_count++;
	pthread_mutex_unlock(&adapter->devices_mutex);

	return d;
}

struct ba_device *ba_device_ref(
		struct ba_device *d) {

	struct ba_adapter *a = d->a;

	pthread_mutex_lock(&a->devices_mutex);
	d->ref_count++;
	pthread_mutex_unlock(&a->devices_mutex);

	return d;
}

void ba_device_destroy(struct ba_device *d) {

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

		pthread_mutex_lock(&d->transports_mutex);

		g_hash_table_iter_init(&iter, d->transports);
		if (!g_hash_table_iter_next(&iter, NULL, (gpointer)&t)) {
			pthread_mutex_unlock(&d->transports_mutex);
			break;
		}

		t->ref_count++;
		g_hash_table_iter_steal(&iter);

		pthread_mutex_unlock(&d->transports_mutex);

		ba_transport_destroy(t);
	}

	ba_device_unref(d);
}

void ba_device_unref(struct ba_device *d) {

	int ref_count;
	struct ba_adapter *a = d->a;

	pthread_mutex_lock(&a->devices_mutex);
	if ((ref_count = --d->ref_count) == 0)
		/* detach device from the adapter */
		g_hash_table_steal(a->devices, &d->addr);
	pthread_mutex_unlock(&a->devices_mutex);

	if (ref_count > 0)
		return;

	debug("Freeing device: %s", batostr_(&d->addr));
	g_assert_cmpint(ref_count, ==, 0);

	ba_adapter_unref(a);
	g_hash_table_unref(d->transports);
	pthread_mutex_destroy(&d->transports_mutex);
	g_free(d->bluez_dbus_path);
	g_free(d->ba_dbus_path);
	free(d);
}
