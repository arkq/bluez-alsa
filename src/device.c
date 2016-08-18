/*
 * BlueALSA - device.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "device.h"

#include <stdlib.h>


struct ba_device *device_new(bdaddr_t *addr, const char *name) {

	struct ba_device *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return NULL;

	bacpy(&d->addr, addr);
	d->name = strdup(name);
	d->transports = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)transport_free);

	return d;
}

void device_free(struct ba_device *d) {
	if (d == NULL)
		return;
	free(d->name);
	free(d);
}

struct ba_transport *device_transport_lookup(GHashTable *devices, const char *key) {

	GHashTableIter iter;
	struct ba_device *d;
	struct ba_transport *t;
	gpointer _key;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, &_key, (gpointer)&d)) {
		if ((t = g_hash_table_lookup(d->transports, key)) != NULL)
			return t;
	}

	return NULL;
}

gboolean device_transport_remove(GHashTable *devices, const char *key) {

	GHashTableIter iter;
	struct ba_device *d;
	gpointer _key;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, &_key, (gpointer)&d)) {
		if (g_hash_table_remove(d->transports, key)) {
			if (g_hash_table_size(d->transports) == 0)
				g_hash_table_iter_remove(&iter);
			return TRUE;
		}
	}

	return FALSE;
}
