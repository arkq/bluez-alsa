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

#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include <glib-object.h>

#include "shared/defs.h"
#include "shared/log.h"

struct dispatch_method_caller_data {
	void (*handler)(GDBusMethodInvocation *);
	GDBusMethodInvocation *invocation;
};

static void *dispatch_method_caller(struct dispatch_method_caller_data *data) {
	data->handler(data->invocation);
	g_free(data);
	return NULL;
}

/**
 * Dispatch incoming D-Bus method call.
 *
 * @param dispatchers NULL-terminated array of dispatchers.
 * @param sender Name of the D-Bus message sender.
 * @param path D-Bus path on which call was made.
 * @param interface D-Bus interface on which call was made.
 * @param method Name of the called D-Bus method.
 * @param invocation D-Bus method invocation structure.
 * @return On success this function returns true. */
bool g_dbus_dispatch_method_call(const GDBusMethodCallDispatcher *dispatchers,
		const char *sender, const char *path, const char *interface,
		const char *method, GDBusMethodInvocation *invocation) {

	const GDBusMethodCallDispatcher *dispatcher;
	for (dispatcher = dispatchers; dispatcher->handler != NULL; dispatcher++) {

		if (dispatcher->sender != NULL && strcmp(dispatcher->sender, sender) != 0)
			continue;
		if (dispatcher->path != NULL && strcmp(dispatcher->path, path) != 0)
			continue;
		if (dispatcher->interface != NULL && strcmp(dispatcher->interface, interface) != 0)
			continue;
		if (dispatcher->method != NULL && strcmp(dispatcher->method, method) != 0)
			continue;

		debug("Called: %s.%s() on %s", interface, method, path);

		if (!dispatcher->asynchronous_call)
			dispatcher->handler(invocation);
		else {

			struct dispatch_method_caller_data data = {
				.handler = dispatcher->handler,
				.invocation = invocation,
			};

			pthread_t thread;
			int ret;

			void *userdata = g_memdup(&data, sizeof(data));
			if ((ret = pthread_create(&thread, NULL,
							PTHREAD_ROUTINE(dispatch_method_caller), userdata)) != 0) {
				error("Couldn't create D-Bus call dispatcher: %s", strerror(ret));
				return false;
			}

			if ((ret = pthread_detach(thread)) != 0) {
				error("Couldn't detach D-Bus call dispatcher: %s", strerror(ret));
				return false;
			}

		}

		return true;
	}

	return false;
}

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
