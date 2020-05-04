/*
 * BlueALSA - dbus.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_DBUS_H_
#define BLUEALSA_DBUS_H_

#include <stdbool.h>

#include <gio/gio.h>
#include <glib.h>

#define DBUS_SERVICE "org.freedesktop.DBus"

#define DBUS_IFACE_DBUS             DBUS_SERVICE
#define DBUS_IFACE_OBJECT_MANAGER   DBUS_SERVICE ".ObjectManager"
#define DBUS_IFACE_PROPERTIES       DBUS_SERVICE ".Properties"

/**
 * Definition of a D-Bus method call dispatcher. */
typedef struct _GDBusMethodCallDispatcher {
	const char *sender;
	const char *path;
	const char *interface;
	const char *method;
	void (*handler)(GDBusMethodInvocation *);
	/* if true, handler will be called in a separate thread */
	bool asynchronous_call;
} GDBusMethodCallDispatcher;

bool g_dbus_dispatch_method_call(const GDBusMethodCallDispatcher *dispatchers,
		const char *sender, const char *path, const char *interface,
		const char *method, GDBusMethodInvocation *invocation);

GVariantIter *g_dbus_get_managed_objects(GDBusConnection *conn,
		const char *name, const char *path, GError **error);

GVariant *g_dbus_get_property(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, const char *property,
		GError **error);
bool g_dbus_set_property(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, const char *property,
		const GVariant *value, GError **error);

#endif
