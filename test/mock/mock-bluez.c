/*
 * mock-bluez.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "mock.h"

#include <stdio.h>
#include <string.h>

#include <bluetooth/bluetooth.h>

#include <gio/gio.h>
#include <glib.h>

#include "ba-config.h"
#include "bluez-iface.h"
#include "utils.h"
#include "shared/defs.h"

/**
 * Bluetooth device name mappings in form of "MAC:name". */
static const char * devices[8] = { NULL };

static GDBusPropertyInfo bluez_iface_device_Adapter = {
	-1, "Adapter", "o", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static GDBusPropertyInfo bluez_iface_device_Alias = {
	-1, "Alias", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static GDBusPropertyInfo bluez_iface_device_Class = {
	-1, "Class", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static GDBusPropertyInfo bluez_iface_device_Icon = {
	-1, "Icon", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static GDBusPropertyInfo bluez_iface_device_Connected = {
	-1, "Connected", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static GDBusPropertyInfo *bluez_iface_device_properties[] = {
	&bluez_iface_device_Adapter,
	&bluez_iface_device_Alias,
	&bluez_iface_device_Class,
	&bluez_iface_device_Icon,
	&bluez_iface_device_Connected,
	NULL,
};

static GDBusInterfaceInfo bluez_iface_device = {
	-1, BLUEZ_IFACE_DEVICE,
	NULL,
	NULL,
	bluez_iface_device_properties,
	NULL,
};

static GDBusMethodInfo bluez_iface_media_transport_Release = {
	-1, "Release", NULL, NULL, NULL
};

static GDBusMethodInfo *bluez_iface_media_transport_methods[] = {
	&bluez_iface_media_transport_Release,
	NULL,
};

static GDBusInterfaceInfo bluez_iface_media_transport = {
	-1, BLUEZ_IFACE_MEDIA_TRANSPORT,
	bluez_iface_media_transport_methods,
	NULL,
	NULL,
	NULL,
};

static GVariant *bluez_device_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *iface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)iface;
	(void)userdata;

	bdaddr_t addr;
	char addrstr[18];
	ba2str(g_dbus_bluez_object_path_to_bdaddr(path, &addr), addrstr);

	if (strcmp(property, "Adapter") == 0)
		return g_variant_new_object_path(MOCK_BLUEZ_ADAPTER_PATH);
	if (strcmp(property, "Class") == 0)
		return g_variant_new_uint32(0x240404);
	if (strcmp(property, "Icon") == 0)
		return g_variant_new_string("audio-card");
	if (strcmp(property, "Connected") == 0)
		return g_variant_new_boolean(TRUE);

	if (strcmp(property, "Alias") == 0) {

		for (size_t i = 0; i < ARRAYSIZE(devices); i++)
			if (devices[i] != NULL &&
					strncmp(devices[i], addrstr, sizeof(addrstr) - 1) == 0)
				return g_variant_new_string(&devices[i][sizeof(addrstr)]);

		if (error != NULL)
			*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
					"Device alias/name not available");
		return NULL;
	}

	g_assert_not_reached();
	return NULL;
}

static const GDBusInterfaceVTable bluez_device_vtable = {
	.get_property = bluez_device_get_property,
};

static void bluez_media_transport_method_call(G_GNUC_UNUSED GDBusConnection *conn,
		G_GNUC_UNUSED const char *sender, G_GNUC_UNUSED const char *path,
		G_GNUC_UNUSED const char *interface, const char *method, G_GNUC_UNUSED GVariant *params,
		GDBusMethodInvocation *invocation, G_GNUC_UNUSED void *userdata) {

	if (strcmp(method, "Release") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	g_assert_not_reached();
}

static const GDBusInterfaceVTable bluez_media_transport_vtable = {
	.method_call = bluez_media_transport_method_call,
};

int mock_bluez_device_name_mapping_add(const char *mapping) {
	for (size_t i = 0; i < ARRAYSIZE(devices); i++)
		if (devices[i] == NULL) {
			devices[i] = strdup(mapping);
			return 0;
		}
	return -1;
}

static void mock_bluez_dbus_name_acquired(GDBusConnection *conn,
		G_GNUC_UNUSED const char *name, void *userdata) {

	g_dbus_connection_register_object(conn, MOCK_BLUEZ_DEVICE_PATH_1,
			&bluez_iface_device, &bluez_device_vtable, NULL, NULL, NULL);
	g_dbus_connection_register_object(conn, MOCK_BLUEZ_DEVICE_PATH_2,
			&bluez_iface_device, &bluez_device_vtable, NULL, NULL, NULL);

	g_dbus_connection_register_object(conn, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_1,
			&bluez_iface_media_transport, &bluez_media_transport_vtable, NULL, NULL, NULL);
	g_dbus_connection_register_object(conn, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_2,
			&bluez_iface_media_transport, &bluez_media_transport_vtable, NULL, NULL, NULL);

	mock_sem_signal(userdata);

}

static GThread *mock_bluez_thread = NULL;
static GMainLoop *mock_bluez_main_loop = NULL;

static void *mock_bluez_loop_run(void *userdata) {

	g_autoptr(GMainContext) context = g_main_context_new();
	mock_bluez_main_loop = g_main_loop_new(context, FALSE);
	g_main_context_push_thread_default(context);

	g_assert(g_bus_own_name_on_connection(config.dbus, BLUEZ_SERVICE,
				G_BUS_NAME_OWNER_FLAGS_NONE, mock_bluez_dbus_name_acquired, NULL,
				userdata, NULL) != 0);

	g_main_loop_run(mock_bluez_main_loop);

	g_main_context_pop_thread_default(context);
	return NULL;
}

void mock_bluez_service_start(void) {
	g_autoptr(GAsyncQueue) ready = g_async_queue_new();
	mock_bluez_thread = g_thread_new("bluez", mock_bluez_loop_run, ready);
	mock_sem_wait(ready);
}

void mock_bluez_service_stop(void) {
	g_main_loop_quit(mock_bluez_main_loop);
	g_main_loop_unref(mock_bluez_main_loop);
	g_thread_join(mock_bluez_thread);
}
