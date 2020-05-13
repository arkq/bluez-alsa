/*
 * BlueALSA - dbus.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "dbus.h"

#include <stdbool.h>
#include <string.h>

/**
 * Get managed objects of a given D-Bus service.
 *
 * @param conn D-Bus connection handler.
 * @param name The name of known D-Bus service.
 * @param path The path which shall be inspected.
 * @param error NULL GError pointer.
 * @return On success this function returns variant iterator with the list of
 *   managed D-Bus objects. After usage, the returned iterator shall be freed
 *   with g_variant_iter_free(). On error, NULL is returned. */
GVariantIter *g_dbus_get_managed_objects(GDBusConnection *conn,
		const char *name, const char *path, GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GVariantIter *objects = NULL;

	msg = g_dbus_message_new_method_call(name, path,
			DBUS_IFACE_OBJECT_MANAGER, "GetManagedObjects");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	g_variant_get(g_dbus_message_get_body(rep), "(a{oa{sa{sv}}})", &objects);

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return objects;
}

/**
 * Get a property of a given D-Bus interface.
 *
 * @param conn D-Bus connection handler.
 * @param service Valid D-Bus service name.
 * @param path Valid D-Bus object path.
 * @param interface Interface with the given property.
 * @param property The property name.
 * @param error NULL GError pointer.
 * @return On success this function returns variant containing property value.
 *   After usage, returned variant shall be freed with g_variant_unref(). On
 *   error, NULL is returned. */
GVariant *g_dbus_get_property(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, const char *property,
		GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GVariant *value = NULL;

	msg = g_dbus_message_new_method_call(service, path, DBUS_IFACE_PROPERTIES, "Get");
	g_dbus_message_set_body(msg, g_variant_new("(ss)", interface, property));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	g_variant_get(g_dbus_message_get_body(rep), "(v)", &value);

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return value;
}

/**
 * Set a property of a given D-Bus interface.
 *
 * @param conn D-Bus connection handler.
 * @param service Valid D-Bus service name.
 * @param path Valid D-Bus object path.
 * @param interface Interface with the given property.
 * @param property The property name.
 * @param value Variant containing property value.
 * @param error NULL GError pointer.
 * @return On success this function returns true. */
bool g_dbus_set_property(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, const char *property,
		const GVariant *value, GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;

	msg = g_dbus_message_new_method_call(service, path, DBUS_IFACE_PROPERTIES, "Set");
	g_dbus_message_set_body(msg, g_variant_new("(ssv)", interface, property, value));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return error == NULL;
}
