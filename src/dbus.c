/*
 * BlueALSA - dbus.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "dbus.h"

#include <stdbool.h>
#include <string.h>

#include <glib-object.h>

#include "shared/log.h"

/**
 * Dispatch incoming D-Bus method call.
 *
 * @param dispatchers NULL-terminated array of dispatchers.
 * @param sender Name of the D-Bus message sender.
 * @param path D-Bus path on which call was made.
 * @param interface D-Bus interface on which call was made.
 * @param method Name of the called D-Bus method.
 * @param invocation D-Bus method invocation structure.
 * @param userdata Data to pass to the handler function.
 * @return On success this function returns true. */
static bool g_dbus_dispatch_method_call(const GDBusMethodCallDispatcher *dispatchers,
		const char *sender, const char *path, const char *interface, const char *method,
		GDBusMethodInvocation *invocation, void *userdata) {

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
		dispatcher->handler(invocation, userdata);

		return true;
	}

	/* make sure that we will not leak the invocation object */
	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method: %s.%s()", interface, method);
	return false;
}

static void interface_skeleton_ex_method_call(G_GNUC_UNUSED GDBusConnection *conn,
		const char *sender, const char *path, const char *interface, const char *method,
		G_GNUC_UNUSED GVariant *params, GDBusMethodInvocation *invocation, void *userdata) {
	GDBusInterfaceSkeletonEx *iface = userdata;
	if (!g_dbus_dispatch_method_call(iface->vtable.dispatchers,
				sender, path, interface, method, invocation, iface->userdata))
		error("Couldn't dispatch D-Bus method call: %s.%s()", interface, method);
}

static GVariant *interface_skeleton_ex_get_property(G_GNUC_UNUSED GDBusConnection *conn,
		G_GNUC_UNUSED const char *sender, G_GNUC_UNUSED const char *path,
		G_GNUC_UNUSED const char *interface, const char *property,
		GError **error, void *userdata) {
	GDBusInterfaceSkeletonEx *iface = userdata;
	return iface->vtable.get_property(property, error, iface->userdata);
}

static gboolean interface_skeleton_ex_set_property(G_GNUC_UNUSED GDBusConnection *conn,
		G_GNUC_UNUSED const char *sender, G_GNUC_UNUSED const char *path,
		G_GNUC_UNUSED const char *interface, const char *property, GVariant *value,
		GError **error, void *userdata) {
	GDBusInterfaceSkeletonEx *iface = userdata;
	return iface->vtable.set_property(property, value, error, iface->userdata);
}

static const GDBusInterfaceVTable interface_skeleton_ex_vtable = {
	.method_call = interface_skeleton_ex_method_call,
	.get_property = interface_skeleton_ex_get_property,
	.set_property = interface_skeleton_ex_set_property,
};

GDBusInterfaceInfo *g_dbus_interface_skeleton_ex_class_get_info(
		GDBusInterfaceSkeleton *interface_skeleton) {
	GDBusInterfaceSkeletonEx *iface = (GDBusInterfaceSkeletonEx *)interface_skeleton;
	return iface->interface_info;
}

GDBusInterfaceVTable *g_dbus_interface_skeleton_ex_class_get_vtable(
		G_GNUC_UNUSED GDBusInterfaceSkeleton *interface_skeleton) {
	return (GDBusInterfaceVTable *)&interface_skeleton_ex_vtable;
}

GVariant *g_dbus_interface_skeleton_ex_class_get_properties(
		GDBusInterfaceSkeleton *interface_skeleton) {
	GDBusInterfaceSkeletonEx *iface = (GDBusInterfaceSkeletonEx *)interface_skeleton;

	if (iface->vtable.get_properties != NULL)
		/* use custom properties getter if provided */
		return iface->vtable.get_properties(iface->userdata);

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	GVariant *v;
	GDBusPropertyInfo **pp = iface->interface_info->properties;
	for (const GDBusPropertyInfo *p = *pp; *pp != NULL; p = *(++pp))
		if ((v = iface->vtable.get_property(p->name, NULL, iface->userdata)) != NULL)
			g_variant_builder_add(&props, "{sv}", p->name, v);

	return g_variant_builder_end(&props);
}

/**
 * Create interface skeleton object of given type.
 *
 * @param interface_skeleton_type The type of interface skeleton.
 * @param interface_info The definition of the D-Bus interface.
 * @param vtable VTable with interface skeleton callback functions.
 * @param userdata Data to pass to callback functions.
 * @param userdata_free_func User data free function called when the
 *   interface skeleton object is destroyed.
 * @return On success, this function returns newly allocated GIO interface
 *   skeleton object, which shall be freed with g_object_unref(). If error
 *   occurs, NULL is returned. */
void *g_dbus_interface_skeleton_ex_new(GType interface_skeleton_type,
		GDBusInterfaceInfo *interface_info, const GDBusInterfaceSkeletonVTable *vtable,
		void *userdata, GDestroyNotify userdata_free_func) {
	GDBusInterfaceSkeletonEx *iface;
	if ((iface = g_object_new(interface_skeleton_type, NULL)) == NULL)
		return NULL;
	g_object_set_data_full(G_OBJECT(iface), "_ud", userdata, userdata_free_func);
	iface->interface_info = interface_info;
	memcpy(&iface->vtable, vtable, sizeof(iface->vtable));
	iface->userdata = userdata;
	return iface;
}

/**
 * Emit properties changed signal for the given D-Bus interface.
 *
 * @param conn D-Bus connection handler.
 * @param path Valid D-Bus object path.
 * @param interface Interface for which properties have changed.
 * @param changed The dictionary variant with changed properties.
 * @param invalidated The array variant with invalidated properties.
 * @param error NULL GError pointer.
 * @return On success this function returns true. */
bool g_dbus_connection_emit_properties_changed(GDBusConnection *conn,
		const char *path, const char *interface, GVariant *changed,
		GVariant *invalidated, GError **error) {
	if (changed == NULL)
		changed = g_variant_new("a{sv}", NULL);
	if (invalidated == NULL)
		invalidated = g_variant_new("as", NULL);
	return g_dbus_connection_emit_signal(conn, NULL, path,
			DBUS_IFACE_PROPERTIES, "PropertiesChanged",
			g_variant_new("(s@a{sv}@as)", interface, changed, invalidated),
			error);
}

/**
 * Get managed objects of a given D-Bus service.
 *
 * @param conn D-Bus connection handler.
 * @param service Valid D-Bus service name.
 * @param path Valid D-Bus object path.
 * @param error NULL GError pointer.
 * @return On success this function returns variant iterator with the list of
 *   managed D-Bus objects. After usage, the returned iterator shall be freed
 *   with g_variant_iter_free(). On error, NULL is returned. */
GVariantIter *g_dbus_get_managed_objects(GDBusConnection *conn, const char *service,
		const char *path, GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GVariantIter *objects = NULL;

	msg = g_dbus_message_new_method_call(service, path,
			DBUS_IFACE_OBJECT_MANAGER, "GetManagedObjects");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL ||
			g_dbus_message_to_gerror(rep, error))
		goto fail;

	g_variant_get(g_dbus_message_get_body(rep), "(a{oa{sa{sv}}})", &objects);

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return objects;
}

/**
 * Get all properties of a given D-Bus interface.
 *
 * @param conn D-Bus connection handler.
 * @param service Valid D-Bus service name.
 * @param path Valid D-Bus object path.
 * @param interface Interface for which properties shall be retrieved.
 * @param error NULL GError pointer.
 * @return On success this function returns variant iterator with the list of
 *	 all properties of the given interface. After usage, the returned iterator
 *   shall be freed with g_variant_iter_free(). On error, NULL is returned. */
GVariantIter *g_dbus_get_properties(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GVariantIter *properties = NULL;

	msg = g_dbus_message_new_method_call(service, path, DBUS_IFACE_PROPERTIES, "GetAll");
	g_dbus_message_set_body(msg, g_variant_new("(s)", interface));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL ||
			g_dbus_message_to_gerror(rep, error))
		goto fail;

	g_variant_get(g_dbus_message_get_body(rep), "(a{sv})", &properties);

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return properties;
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
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL ||
			g_dbus_message_to_gerror(rep, error))
		goto fail;

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
	bool rv = false;

	msg = g_dbus_message_new_method_call(service, path, DBUS_IFACE_PROPERTIES, "Set");
	g_dbus_message_set_body(msg, g_variant_new("(ssv)", interface, property, value));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL ||
			g_dbus_message_to_gerror(rep, error))
		goto fail;

	rv = true;

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return rv;
}
