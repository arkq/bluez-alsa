/*
 * dbus.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdio.h>
#include <gio/gio.h>
#include <glib.h>

/**
 * Open new testing D-Bus connection. */
GDBusConnection *g_test_dbus_connection_new_sync(GError **error) {
	GTestDBus *dbus;
	g_test_dbus_up(dbus = g_test_dbus_new(G_TEST_DBUS_NONE));
	fprintf(stderr, "DBUS_SYSTEM_BUS_ADDRESS=%s\n", g_test_dbus_get_bus_address(dbus));
	return g_dbus_connection_new_for_address_sync(
			g_test_dbus_get_bus_address(dbus),
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
			NULL, NULL, error);
}
