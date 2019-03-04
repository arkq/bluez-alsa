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
#include <stdio.h>

#include "bluealsa.h"

static guint g_bdaddr_hash(gconstpointer v) {
	const bdaddr_t *ba = (const bdaddr_t *)v;
	return ((uint32_t *)ba->b)[0] * ((uint16_t *)ba->b)[2];
}

static gboolean g_bdaddr_equal(gconstpointer v1, gconstpointer v2) {
	return bacmp(v1, v2) == 0;
}

struct ba_adapter *ba_adapter_new(int dev_id, const char *name) {

	struct ba_adapter *a;

	/* make sure we are within array boundaries */
	if (dev_id < 0 || dev_id >= HCI_MAX_DEV) {
		errno = EINVAL;
		return NULL;
	}

	if ((a = calloc(1, sizeof(*a))) == NULL)
		return NULL;

	a->hci_dev_id = dev_id;

	if (name != NULL)
		strncpy(a->hci_name, name, sizeof(a->hci_name) - 1);
	else
		sprintf(a->hci_name, "hci%d", dev_id);

	pthread_mutex_init(&a->devices_mutex, NULL);
	a->devices = g_hash_table_new_full(g_bdaddr_hash, g_bdaddr_equal, NULL, NULL);

	config.adapters[a->hci_dev_id] = a;
	return a;
}

struct ba_adapter *ba_adapter_lookup(int dev_id) {
	if (dev_id >= 0 && dev_id < HCI_MAX_DEV)
		return config.adapters[dev_id];
	return NULL;
}

void ba_adapter_free(struct ba_adapter *a) {

	/* detach adapter from global configuration */
	config.adapters[a->hci_dev_id] = NULL;

	if (a->devices != NULL) {

		GHashTableIter iter;
		struct ba_device *d;

		g_hash_table_iter_init(&iter, a->devices);
		while (g_hash_table_iter_next(&iter, NULL, (gpointer)&d))
			ba_device_free(d);

		g_hash_table_unref(a->devices);
	}

	pthread_mutex_destroy(&a->devices_mutex);

	free(a);
}
