/*
 * mock-upower.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "mock.h"

#include <stdbool.h>
#include <stddef.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "upower.h"

#include "dbus-ifaces.h"

/* Global UPower mock server object manager. */
static GDBusObjectManagerServer *server = NULL;
/* Display device exposed by the UPower mock server. */
static MockFreedesktopUPowerDevice *display_device = NULL;

static MockFreedesktopUPowerDevice *mock_upower_device_add(const char *path) {

	MockFreedesktopUPowerDevice *device = mock_freedesktop_upower_device_skeleton_new();
	mock_freedesktop_upower_device_set_is_present(device, TRUE);
	mock_freedesktop_upower_device_set_percentage(device, 100.00);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(device));
	g_dbus_object_manager_server_export(server, skeleton);

	return device;
}

int mock_upower_display_device_set_is_present(bool present) {
	mock_freedesktop_upower_device_set_is_present(display_device, present);
	return 0;
}

int mock_upower_display_device_set_percentage(double percentage) {
	mock_freedesktop_upower_device_set_percentage(display_device, percentage);
	return 0;
}

static void mock_dbus_name_acquired(GDBusConnection *conn,
		G_GNUC_UNUSED const char *name, void *userdata) {

	server = g_dbus_object_manager_server_new("/");
	display_device = mock_upower_device_add(UPOWER_PATH_DISPLAY_DEVICE);

	g_dbus_object_manager_server_set_connection(server, conn);
	mock_sem_signal(userdata);

}

static GThread *mock_thread = NULL;
static GMainLoop *mock_main_loop = NULL;
static unsigned int mock_owner_id = 0;

static void *mock_loop_run(void *userdata) {

	g_autoptr(GMainContext) context = g_main_context_new();
	mock_main_loop = g_main_loop_new(context, FALSE);
	g_main_context_push_thread_default(context);

	g_autoptr(GDBusConnection) conn = mock_dbus_connection_new_sync(NULL);
	g_assert((mock_owner_id = g_bus_own_name_on_connection(conn,
					UPOWER_SERVICE, G_BUS_NAME_OWNER_FLAGS_NONE,
					mock_dbus_name_acquired, NULL, userdata, NULL)) != 0);

	g_main_loop_run(mock_main_loop);

	g_main_context_pop_thread_default(context);
	return NULL;
}

void mock_upower_service_start(void) {
	g_autoptr(GAsyncQueue) ready = g_async_queue_new();
	mock_thread = g_thread_new("UPower", mock_loop_run, ready);
	mock_sem_wait(ready);
}

void mock_upower_service_stop(void) {

	g_bus_unown_name(mock_owner_id);

	g_main_loop_quit(mock_main_loop);
	g_main_loop_unref(mock_main_loop);
	g_thread_join(mock_thread);

	g_object_unref(server);

}
