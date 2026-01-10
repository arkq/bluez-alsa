/*
 * BlueALSA - dbus.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_DBUS_H_
#define BLUEALSA_DBUS_H_

#include <stdbool.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#define DBUS_SERVICE "org.freedesktop.DBus"

#define DBUS_IFACE_DBUS             DBUS_SERVICE
#define DBUS_IFACE_INTROSPECTABLE   DBUS_SERVICE ".Introspectable"
#define DBUS_IFACE_OBJECT_MANAGER   DBUS_SERVICE ".ObjectManager"
#define DBUS_IFACE_PROPERTIES       DBUS_SERVICE ".Properties"

/* Compatibility patch for glib < 2.42. */
#if !GLIB_CHECK_VERSION(2, 42, 0)
# define G_DBUS_ERROR_UNKNOWN_OBJECT G_DBUS_ERROR_FAILED
#endif

/**
 * Definition of a D-Bus method call dispatcher. */
typedef struct _GDBusMethodCallDispatcher {
	const char *sender;
	const char *path;
	const char *interface;
	const char *method;
	void (*handler)(GDBusMethodInvocation *, void *);
} GDBusMethodCallDispatcher;

typedef struct _GDBusInterfaceSkeletonVTable {
	const GDBusMethodCallDispatcher *dispatchers;
	GVariant *(*get_properties)(void *userdata);
	GVariant *(*get_property)(const char *property, GError **error, void *userdata);
	bool (*set_property)(const char *property, GVariant *value, GError **error, void *userdata);
} GDBusInterfaceSkeletonVTable;

/**
 * Definition for interface skeleton with callbacks. */
typedef struct _GDBusInterfaceSkeletonEx {
	GDBusInterfaceSkeleton parent;
	GDBusInterfaceInfo *interface_info;
	GDBusInterfaceSkeletonVTable vtable;
	/* user data passed to callback functions */
	void *userdata;
} GDBusInterfaceSkeletonEx;

GDBusInterfaceInfo *g_dbus_interface_skeleton_ex_class_get_info(
		GDBusInterfaceSkeleton *interface_skeleton);

GDBusInterfaceVTable *g_dbus_interface_skeleton_ex_class_get_vtable(
		GDBusInterfaceSkeleton *interface_skeleton);

GVariant *g_dbus_interface_skeleton_ex_class_get_properties(
		GDBusInterfaceSkeleton *interface_skeleton);

void *g_dbus_interface_skeleton_ex_new(GType interface_skeleton_type,
		GDBusInterfaceInfo *interface_info, const GDBusInterfaceSkeletonVTable *vtable,
		void *userdata, GDestroyNotify userdata_free_func);

/**
 * Create a new message bus GDBusConnection for the given address. */
static inline GDBusConnection * g_dbus_connection_new_for_address_simple_sync(
		const char * address, GError ** error) {
	return g_dbus_connection_new_for_address_sync(address,
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
			NULL, NULL, error);
}

bool g_dbus_connection_emit_properties_changed(GDBusConnection *conn,
		const char *path, const char *interface, GVariant *changed,
		GVariant *invalidated, GError **error);

char * g_dbus_get_unique_name_sync(GDBusConnection * conn,
		const char * service);

GVariantIter * g_dbus_get_managed_objects_sync(GDBusConnection * conn,
		const char * service, const char * path, GError ** error);

GVariantIter * g_dbus_get_properties_sync(GDBusConnection * conn,
		const char * service, const char * path, const char * interface,
		GError ** error);

void g_dbus_get_property(GDBusConnection * conn, const char * service,
		const char * path, const char * interface, const char * property,
		GAsyncReadyCallback callback, void * userdata);
GVariant * g_dbus_get_property_finish(GDBusConnection * conn,
		GAsyncResult * result, GError ** error);

bool g_dbus_set_property_sync(GDBusConnection * conn, const char * service,
		const char * path, const char * interface, const char * property,
		const GVariant * value, GError ** error);

#endif
