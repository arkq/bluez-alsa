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

#include <stdlib.h>

#include "ba-transport.h"
#include "bluealsa.h"
#include "ctl.h"

struct ba_device *ba_device_new(
		int hci_dev_id,
		const bdaddr_t *addr,
		const char *name) {

	struct ba_device *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return NULL;

	d->hci_dev_id = hci_dev_id;
	bacpy(&d->addr, addr);
	strncpy(d->name, name, sizeof(d->name) - 1);

	d->transports = g_hash_table_new_full(g_str_hash, g_str_equal,
			NULL, (GDestroyNotify)transport_free);

	return d;
}

void ba_device_free(struct ba_device *d) {

	if (d == NULL)
		return;

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

		transport_free(t);
	}

	g_hash_table_unref(d->transports);
	free(d);
}

void ba_device_set_battery_level(struct ba_device *d, uint8_t value) {
	d->battery.enabled = true;
	d->battery.level = value;
	bluealsa_ctl_send_event(config.ctl, BA_EVENT_BATTERY, &d->addr, 0);
}
