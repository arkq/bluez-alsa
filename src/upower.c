/*
 * BlueALSA - upower.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "upower.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "bluealsa.h"
#include "dbus.h"
#include "utils.h"
#include "shared/log.h"

/**
 * Get initial setup from UPower service.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int upower_initialize(void) {

	GVariant *present = g_dbus_get_property(config.dbus, UPOWER_SERVICE,
			UPOWER_PATH_DISPLAY_DEVICE, UPOWER_IFACE_DEVICE, "IsPresent", NULL);
	GVariant *percentage = g_dbus_get_property(config.dbus, UPOWER_SERVICE,
			UPOWER_PATH_DISPLAY_DEVICE, UPOWER_IFACE_DEVICE, "Percentage", NULL);

	if (present != NULL) {
		config.battery.available = g_variant_get_boolean(present);
		g_variant_unref(present);
	}

	if (percentage != NULL) {
		config.battery.level = g_variant_get_double(percentage);
		g_variant_unref(percentage);
	}

	return 0;
}

static void upower_signal_display_device_changed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface_, signal);
	(void)conn;
	(void)sender;
	(void)path;
	(void)userdata;

	bool updated = false;
	GVariantIter *properties = NULL;
	const char *interface;
	const char *property;
	GVariant *value;

	g_variant_get(params, "(&sa{sv}as)", &interface, &properties, NULL);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {

		if (strcmp(property, "IsPresent") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BOOLEAN, property)) {
			config.battery.available = g_variant_get_boolean(value);
			updated = true;
		}
		else if (strcmp(property, "Percentage") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_DOUBLE, property)) {
			config.battery.level = g_variant_get_double(value);
			updated = true;
		}

		g_variant_unref(value);
	}

	if (updated) {

		GHashTableIter iter_d, iter_t;
		struct ba_adapter *a;
		struct ba_device *d;
		struct ba_transport *t;
		size_t i;

		/* find every RFCOMM profile and send update battery signal */
		for (i = 0; i < HCI_MAX_DEV; i++) {
			if ((a = ba_adapter_lookup(i)) == NULL)
				continue;
			pthread_mutex_lock(&a->devices_mutex);
			g_hash_table_iter_init(&iter_d, a->devices);
			while (g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d)) {
				pthread_mutex_lock(&d->transports_mutex);
				g_hash_table_iter_init(&iter_t, d->transports);
				while (g_hash_table_iter_next(&iter_t, NULL, (gpointer)&t))
					if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO &&
							t->sco.rfcomm != NULL)
						ba_rfcomm_send_signal(t->sco.rfcomm, BA_RFCOMM_SIGNAL_UPDATE_BATTERY);
				pthread_mutex_unlock(&d->transports_mutex);
			}
			pthread_mutex_unlock(&a->devices_mutex);
			ba_adapter_unref(a);
		}

	}

	g_variant_iter_free(properties);
}

/**
 * Subscribe to UPower service signals.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int upower_subscribe_signals(void) {

	g_dbus_connection_signal_subscribe(config.dbus, UPOWER_SERVICE,
			DBUS_IFACE_PROPERTIES, "PropertiesChanged", UPOWER_PATH_DISPLAY_DEVICE,
			NULL, G_DBUS_SIGNAL_FLAGS_NONE, upower_signal_display_device_changed, NULL, NULL);

	return 0;
}
