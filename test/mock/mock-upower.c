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
	struct MockService *service = userdata;

	server = g_dbus_object_manager_server_new("/");
	display_device = mock_upower_device_add(UPOWER_PATH_DISPLAY_DEVICE);

	g_dbus_object_manager_server_set_connection(server, conn);
	mock_sem_signal(service->ready);

}

static struct MockService service = {
	.name = UPOWER_SERVICE,
	.name_acquired_cb = mock_dbus_name_acquired,
};

void mock_upower_service_start(void) {
	mock_service_start(&service);
}

void mock_upower_service_stop(void) {
	mock_service_stop(&service);
	g_object_unref(server);
}
