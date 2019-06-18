/*
 * BlueALSA - ba-adapter.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ba-adapter.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include "ba-device.h"
#include "bluealsa.h"
#include "utils.h"
#include "shared/log.h"

static guint g_bdaddr_hash(gconstpointer v) {
	const bdaddr_t *ba = (const bdaddr_t *)v;
	return ((uint32_t *)ba->b)[0] * ((uint16_t *)ba->b)[2];
}

static gboolean g_bdaddr_equal(gconstpointer v1, gconstpointer v2) {
	return bacmp(v1, v2) == 0;
}

struct ba_adapter *ba_adapter_new(int dev_id) {

	struct ba_adapter *a;

	/* make sure we are within array boundaries */
	if (dev_id < 0 || dev_id >= HCI_MAX_DEV) {
		errno = EINVAL;
		return NULL;
	}

	if ((a = calloc(1, sizeof(*a))) == NULL)
		return NULL;

	a->hci_dev_id = dev_id;
	sprintf(a->hci_name, "hci%d", dev_id);
	a->ref_count = 1;

	sprintf(a->ba_dbus_path, "/org/bluealsa/%s", a->hci_name);
	g_variant_sanitize_object_path(a->ba_dbus_path);
	sprintf(a->bluez_dbus_path, "/org/bluez/%s", a->hci_name);
	g_variant_sanitize_object_path(a->bluez_dbus_path);

	pthread_mutex_init(&a->devices_mutex, NULL);
	a->devices = g_hash_table_new_full(g_bdaddr_hash, g_bdaddr_equal, NULL, NULL);

	pthread_mutex_lock(&config.adapters_mutex);
	config.adapters[a->hci_dev_id] = a;
	pthread_mutex_unlock(&config.adapters_mutex);

	return a;
}

struct ba_adapter *ba_adapter_lookup(int dev_id) {

	if (dev_id < 0 || dev_id >= HCI_MAX_DEV)
		return NULL;

	struct ba_adapter *a;

	pthread_mutex_lock(&config.adapters_mutex);
	if ((a = config.adapters[dev_id]) != NULL)
		a->ref_count++;
	pthread_mutex_unlock(&config.adapters_mutex);

	return a;
}

struct ba_adapter *ba_adapter_ref(struct ba_adapter *a) {
	pthread_mutex_lock(&config.adapters_mutex);
	a->ref_count++;
	pthread_mutex_unlock(&config.adapters_mutex);
	return a;
}

void ba_adapter_destroy(struct ba_adapter *a) {

	/* XXX: Modification-safe remove-all loop.
	 *
	 * Before calling ba_device_destroy() we have to unlock mutex, so
	 * in theory it is possible that someone will modify devices hash
	 * table over which we are iterating. Since, the iterator uses an
	 * internal cache, we have to reinitialize it after every unlock. */
	for (;;) {

		GHashTableIter iter;
		struct ba_device *d;

		pthread_mutex_lock(&a->devices_mutex);

		g_hash_table_iter_init(&iter, a->devices);
		if (!g_hash_table_iter_next(&iter, NULL, (gpointer)&d)) {
			pthread_mutex_unlock(&a->devices_mutex);
			break;
		}

		d->ref_count++;
		g_hash_table_iter_steal(&iter);

		pthread_mutex_unlock(&a->devices_mutex);

		ba_device_destroy(d);
	}

	ba_adapter_unref(a);
}

void ba_adapter_unref(struct ba_adapter *a) {

	int ref_count;

	pthread_mutex_lock(&config.adapters_mutex);
	if ((ref_count = --a->ref_count) == 0)
		/* detach adapter from global configuration */
		config.adapters[a->hci_dev_id] = NULL;
	pthread_mutex_unlock(&config.adapters_mutex);

	if (ref_count > 0)
		return;

	debug("Freeing adapter: %s", a->hci_name);

	g_hash_table_unref(a->devices);
	pthread_mutex_destroy(&a->devices_mutex);
	free(a);
}
