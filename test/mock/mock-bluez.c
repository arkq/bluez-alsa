/*
 * mock-bluez.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "mock.h"

#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-config.h"
#include "bluez-iface.h"
#include "dbus.h"
#include "shared/defs.h"

#include "mock-bluez-iface.h"

/* Bluetooth device name mappings in form of "MAC:name". */
static const char * devices[8] = { NULL };
/* Global BlueZ mock server object manager. */
static GDBusObjectManagerServer *server = NULL;

int mock_bluez_device_name_mapping_add(const char *mapping) {
	for (size_t i = 0; i < ARRAYSIZE(devices); i++)
		if (devices[i] == NULL) {
			devices[i] = strdup(mapping);
			return 0;
		}
	return -1;
}

static void mock_bluez_adapter_add(const char *path, const char *address) {

	g_autoptr(MockBluezAdapter1) adapter = mock_bluez_adapter1_skeleton_new();
	mock_bluez_adapter1_set_address(adapter, address);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(adapter));
	g_dbus_object_manager_server_export(server, skeleton);

}

static void mock_bluez_device_add(const char *path, const char *adapter, const char *address) {

	g_autoptr(MockBluezDevice1) device = mock_bluez_device1_skeleton_new();
	mock_bluez_device1_set_adapter(device, adapter);
	mock_bluez_device1_set_alias(device, address);
	mock_bluez_device1_set_icon(device, "audio-card");

	for (size_t i = 0; i < ARRAYSIZE(devices); i++)
		if (devices[i] != NULL &&
				strncmp(devices[i], address, strlen(address)) == 0)
			mock_bluez_device1_set_alias(device, &devices[i][strlen(address) + 1]);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(device));
	g_dbus_object_manager_server_export(server, skeleton);

}

static gboolean mock_bluez_media_transport_release_handler(MockBluezMediaTransport1 *transport,
		GDBusMethodInvocation *invocation, G_GNUC_UNUSED void *userdata) {
	mock_bluez_media_transport1_complete_release(transport, invocation);

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&props, "{sv}", "State", g_variant_new_string("idle"));
	GDBusConnection *conn = g_dbus_method_invocation_get_connection(invocation);
	const char *path = g_dbus_method_invocation_get_object_path(invocation);
	const char *iface = g_dbus_method_invocation_get_interface_name(invocation);
	g_dbus_connection_emit_properties_changed(conn, path, iface,
			g_variant_builder_end(&props), NULL, NULL);

	return TRUE;
}

static void mock_bluez_media_transport_add(const char *path, const char *device) {

	g_autoptr(MockBluezMediaTransport1) transport = mock_bluez_media_transport1_skeleton_new();
	mock_bluez_media_transport1_set_device(transport, device);

	g_signal_connect(transport, "handle-release",
			G_CALLBACK(mock_bluez_media_transport_release_handler), NULL);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(transport));
	g_dbus_object_manager_server_export(server, skeleton);

}

static void mock_bluez_dbus_name_acquired(GDBusConnection *conn,
		G_GNUC_UNUSED const char *name, void *userdata) {

	server = g_dbus_object_manager_server_new("/");

	mock_bluez_adapter_add(MOCK_BLUEZ_ADAPTER_PATH, "00:00:11:11:22:22");
	mock_bluez_device_add(MOCK_BLUEZ_DEVICE_PATH_1, MOCK_BLUEZ_ADAPTER_PATH, MOCK_DEVICE_1);
	mock_bluez_media_transport_add(MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_1, MOCK_BLUEZ_DEVICE_PATH_1);
	mock_bluez_device_add(MOCK_BLUEZ_DEVICE_PATH_2, MOCK_BLUEZ_ADAPTER_PATH, MOCK_DEVICE_2);
	mock_bluez_media_transport_add(MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_2, MOCK_BLUEZ_DEVICE_PATH_2);

	g_dbus_object_manager_server_set_connection(server, conn);
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
