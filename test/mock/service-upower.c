/*
 * service-upower.c
 * SPDX-FileCopyrightText: 2024-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "service.h"

#include <stdbool.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "upower.h"

#include "dbus-ifaces.h"

typedef struct UPowerMockServicePriv {
	/* Public members. */
	UPowerMockService p;
	/* Global UPower object manager. */
	GDBusObjectManagerServer * server;
	/* Display device exposed by the UPower service. */
	MockFreedesktopUPowerDevice * display_device;
} UPowerMockServicePriv;

static MockFreedesktopUPowerDevice * device_new(
		GDBusObjectManagerServer * server, const char * path) {

	MockFreedesktopUPowerDevice * device = mock_freedesktop_upower_device_skeleton_new();
	mock_freedesktop_upower_device_set_is_present(device, TRUE);
	mock_freedesktop_upower_device_set_percentage(device, 100.00);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(device));
	g_dbus_object_manager_server_export(server, skeleton);

	return device;
}

void mock_upower_service_display_device_set_is_present(UPowerMockService * srv,
		bool present) {
	UPowerMockServicePriv * self = (UPowerMockServicePriv *)srv;
	mock_freedesktop_upower_device_set_is_present(self->display_device, present);
}

void mock_upower_service_display_device_set_percentage(UPowerMockService * srv,
		double percentage) {
	UPowerMockServicePriv * self = (UPowerMockServicePriv *)srv;
	mock_freedesktop_upower_device_set_percentage(self->display_device, percentage);
}

static void name_acquired(GDBusConnection * conn,
		G_GNUC_UNUSED const char * name, void * userdata) {
	UPowerMockServicePriv * self = userdata;

	self->server = g_dbus_object_manager_server_new("/");
	self->display_device = device_new(self->server, UPOWER_PATH_DISPLAY_DEVICE);
	g_dbus_object_manager_server_set_connection(self->server, conn);

	mock_service_ready(self);
}

static void service_free(void * service) {
	g_autofree UPowerMockServicePriv * self = service;
	g_object_unref(self->display_device);
	g_object_unref(self->server);
}

UPowerMockService * mock_upower_service_new(void) {
	UPowerMockServicePriv * self = g_new0(UPowerMockServicePriv, 1);
	self->p.service.name = UPOWER_SERVICE;
	self->p.service.name_acquired_cb = name_acquired;
	self->p.service.free = service_free;
	return (UPowerMockService *)self;
}
