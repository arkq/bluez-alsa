/*
 * BlueALSA - bt-gatt.c
 * SPDX-FileCopyrightText: 2025-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bt-gatt.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "dbus.h"
#include "shared/log.h"

struct _BluetoothGATTApplication {
	GObject parent;
	char * path;
	/* The object manager server holding all GATT objects. */
	GDBusObjectManagerServer * manager;
	/* Registration callback and userdata. */
	GAsyncReadyCallback register_cb;
	void * register_userdata;
	bool registered;
};

struct _BluetoothGATTService {
	GObject parent;
	/* The GATT application managing this service. The pointer is not owned
	 * because if set the app's object manager server owns this service. */
	BluetoothGATTApplication * app;
	/* Export path relative to the application path. */
	char * path;
	/* Full export path (application + service path). */
	char * path_full;
	/* Service UUID. */
	char uuid[37];
	bool primary;
};

struct _BluetoothGATTCharacteristic {
	GObject parent;
	/* The GATT service managing this characteristic. The pointer is not owned
	 * because if set the app's object manager server owns this characteristic. */
	BluetoothGATTService * service;
	/* Export path relative to the service path. */
	char * path;
	/* Full export path (application + service + characteristic path). */
	char * path_full;
	/* Characteristic UUID. */
	char uuid[37];
	/* Flags with capabilities. */
	char ** flags;
	/* Callback handlers for GATT operations. */
	BluetoothGATTCharacteristicCallback read_cb;
	void * read_cb_userdata;
	BluetoothGATTCharacteristicCallback acquire_notify_cb;
	void * acquire_notify_cb_userdata;
	BluetoothGATTCharacteristicCallback acquire_write_cb;
	void * acquire_write_cb_userdata;
	/* Notify acquisition state. */
	bool notify_acquired;
	/* Write acquisition state. */
	bool write_acquired;
};

G_DEFINE_TYPE(BluetoothGATTApplication, bluetooth_gatt_application, G_TYPE_OBJECT)
G_DEFINE_TYPE(BluetoothGATTService, bluetooth_gatt_service, G_TYPE_OBJECT)
G_DEFINE_TYPE(BluetoothGATTCharacteristic, bluetooth_gatt_characteristic, G_TYPE_OBJECT)

static void bluetooth_gatt_application_init(
		G_GNUC_UNUSED BluetoothGATTApplication * app) {
}

static void bluetooth_gatt_application_finalize(GObject * object) {
	BluetoothGATTApplication * app = BLUETOOTH_GATT_APPLICATION(object);
	debug("Freeing GATT application: %s", app->path);
	g_object_unref(app->manager);
	g_free(app->path);
	G_OBJECT_CLASS(bluetooth_gatt_application_parent_class)->finalize(object);
}

static void bluetooth_gatt_application_class_init(
		BluetoothGATTApplicationClass * _class) {
	GObjectClass * object_class = G_OBJECT_CLASS(_class);
	object_class->finalize = bluetooth_gatt_application_finalize;
}

/**
 * Create new BlueZ GATT application.
 *
 * @param path Export path for the application.
 * @return New BlueZ GATT application. The returned object should be freed
 *   with g_object_unref(). */
BluetoothGATTApplication * bluetooth_gatt_application_new(const char * path) {
	BluetoothGATTApplication * app = g_object_new(BLUETOOTH_TYPE_GATT_APPLICATION, NULL);
	app->manager = g_dbus_object_manager_server_new(path);
	app->path = g_strdup(path);
	return app;
}

/**
 * Get the object manager server of the GATT application.
 *
 * The returned object is owned by the GATT application. Caller should take
 * a reference if it needs to keep the object beyond the lifetime of the GATT
 * application.
 *
 * @param app The GATT application.
 * @return The object manager server. */
GDBusObjectManagerServer * bluetooth_gatt_application_get_object_manager_server(
		BluetoothGATTApplication * app) {
	return app->manager;
}

static GVariant * gatt_service_iface_get_property(
		const char * property, G_GNUC_UNUSED GError ** error, void * userdata) {
	const BluetoothGATTService * srv = userdata;

	if (strcmp(property, "UUID") == 0)
		return g_variant_new_string(srv->uuid);
	if (strcmp(property, "Primary") == 0)
		return g_variant_new_boolean(srv->primary);

	g_assert_not_reached();
	return NULL;
}

/**
 * Add GATT service to the GATT application.
 *
 * @param app The GATT application.
 * @param srv The GATT service to add. */
void bluetooth_gatt_application_add_service(
		BluetoothGATTApplication * app,
		BluetoothGATTService * srv) {

	static const GDBusInterfaceSkeletonVTable vtable = {
		.get_property = gatt_service_iface_get_property,
	};

	srv->path_full = g_strdup_printf("%s%s", app->path, srv->path);
	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(srv->path_full);

	g_autoptr(OrgBluezGattService1Skeleton) ifs;
	ifs = org_bluez_gatt_service1_skeleton_new(&vtable, g_object_ref(srv), g_object_unref);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(ifs));

	g_dbus_object_manager_server_export(app->manager, skeleton);
	srv->app = app;

}

static void gatt_characteristic_handle_read_value(
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothGATTCharacteristic * chr = userdata;
	chr->read_cb(chr, inv, chr->read_cb_userdata);
}

static void gatt_characteristic_handle_acquire_notify(
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothGATTCharacteristic * chr = userdata;
	chr->notify_acquired = chr->acquire_notify_cb(chr, inv, chr->acquire_notify_cb_userdata);
}

static void gatt_characteristic_handle_acquire_write(
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothGATTCharacteristic * chr = userdata;
	chr->write_acquired = chr->acquire_write_cb(chr, inv, chr->acquire_write_cb_userdata);
}

static GVariant * gatt_characteristic_iface_get_property(
		const char * property, G_GNUC_UNUSED GError ** error, void * userdata) {
	const BluetoothGATTCharacteristic * chr = userdata;
	const BluetoothGATTService * srv = chr->service;

	if (strcmp(property, "UUID") == 0)
		return g_variant_new_string(chr->uuid);
	if (strcmp(property, "Service") == 0) {
		char path[256];
		snprintf(path, sizeof(path), "%s%s", srv->app->path, srv->path);
		return g_variant_new_object_path(path);
	}
	if (strcmp(property, "Flags") == 0)
		return g_variant_new_strv((const char * const *)chr->flags, chr->flags == NULL ? 0 : -1);
	if (strcmp(property, "WriteAcquired") == 0)
		return g_variant_new_boolean(chr->write_acquired);
	if (strcmp(property, "NotifyAcquired") == 0)
		return g_variant_new_boolean(chr->notify_acquired);

	g_assert_not_reached();
	return NULL;
}

/**
 * Add GATT characteristic to the GATT service.
 *
 * @param app The GATT application.
 * @param srv The GATT service.
 * @param chr The GATT characteristic to add. */
void bluetooth_gatt_application_add_service_characteristic(
		BluetoothGATTApplication * app,
		BluetoothGATTService * srv,
		BluetoothGATTCharacteristic * chr) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "ReadValue",
			.sender = bluez_dbus_unique_name,
			.handler = gatt_characteristic_handle_read_value },
		{ .method = "AcquireNotify",
			.sender = bluez_dbus_unique_name,
			.handler = gatt_characteristic_handle_acquire_notify },
		{ .method = "AcquireWrite",
			.sender = bluez_dbus_unique_name,
			.handler = gatt_characteristic_handle_acquire_write },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
		.get_property = gatt_characteristic_iface_get_property,
	};

	chr->path_full = g_strdup_printf("%s%s", srv->path_full, chr->path);
	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(chr->path_full);

	g_autoptr(OrgBluezGattCharacteristic1Skeleton) ifs;
	ifs = org_bluez_gatt_characteristic1_skeleton_new(&vtable, g_object_ref(chr), g_object_unref);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(ifs));

	g_dbus_object_manager_server_export(app->manager, skeleton);
	chr->service = srv;

}

void bluetooth_gatt_application_set_connection(
		BluetoothGATTApplication * app,
		GDBusConnection * conn) {
	g_dbus_object_manager_server_set_connection(app->manager, conn);
}

static void register_application_finish(
		GObject * source, GAsyncResult * result, void * userdata) {
	g_autoptr(BluetoothGATTApplication) app = userdata;

	g_autoptr(GDBusMessage) rep;
	g_autoptr(GError) err = NULL;
	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	g_autoptr(GTask) task;
	task = g_task_new(app, NULL, app->register_cb, app->register_userdata);

	if (err != NULL) {
		error("Couldn't register GATT application [%s]: %s", app->path, err->message);
		g_task_return_error(task, g_steal_pointer(&err));
	}
	else {
		app->registered = true;
		/* NOTE: The reference to the Bluetooth GATT object shall be held
		 *       as long as the application is registered in BlueZ. */
		g_task_return_boolean(task, TRUE);
	}

}

/**
 * Register the GATT application on the specified adapter.
 *
 * @param app The GATT application.
 * @param adapter The adapter on which to register the application.
 * @param callback The registration completion callback.
 * @param userdata The user data for the registration completion callback. */
void bluetooth_gatt_application_register(
		BluetoothGATTApplication * app,
		struct ba_adapter * adapter,
		GAsyncReadyCallback callback,
		void * userdata) {

	g_autoptr(GDBusConnection) conn = g_dbus_object_manager_server_get_connection(app->manager);
	g_autoptr(GDBusMessage) msg = g_dbus_message_new_method_call(BLUEZ_SERVICE,
			adapter->bluez_dbus_path, BLUEZ_IFACE_GATT_MANAGER, "RegisterApplication");
	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", app->path, NULL));

	app->register_cb = callback;
	app->register_userdata = userdata;

	debug("Registering GATT application: %s", app->path);
	g_dbus_connection_send_message_with_reply(conn, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
			register_application_finish, g_object_ref(app));

}

bool bluetooth_gatt_application_register_finish(
		BluetoothGATTApplication * app,
		GAsyncResult * result,
		GError ** error) {
	app->register_cb = NULL;
	app->register_userdata = NULL;
	return g_task_propagate_boolean(G_TASK(result), error);
}

static void bluetooth_gatt_service_init(
		G_GNUC_UNUSED BluetoothGATTService * srv) {
}

static void bluetooth_gatt_service_finalize(GObject * object) {
	BluetoothGATTService * srv = BLUETOOTH_GATT_SERVICE(object);
	debug("Freeing GATT service: %s", srv->path_full != NULL ? srv->path_full : srv->path);
	g_free(srv->path);
	g_free(srv->path_full);
	G_OBJECT_CLASS(bluetooth_gatt_service_parent_class)->finalize(object);
}

static void bluetooth_gatt_service_class_init(
		BluetoothGATTServiceClass * _class) {
	GObjectClass * object_class = G_OBJECT_CLASS(_class);
	object_class->finalize = bluetooth_gatt_service_finalize;
}

/**
 * Create new BlueZ GATT service.
 *
 * @param path Export path relative to the application path.
 * @param uuid Service UUID.
 * @param primary Whether the service is primary.
 * @return New BlueZ GATT service. The returned object should be freed
 *   with g_object_unref(). */
BluetoothGATTService * bluetooth_gatt_service_new(
		const char * path,
		const char * uuid,
		bool primary) {
	BluetoothGATTService * srv = g_object_new(BLUETOOTH_TYPE_GATT_SERVICE, NULL);
	strncpy(srv->uuid, uuid, sizeof(srv->uuid) - 1);
	srv->path = g_strdup(path);
	srv->primary = primary;
	return srv;
}

static void bluetooth_gatt_characteristic_init(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr) {
}

static void bluetooth_gatt_characteristic_finalize(GObject * object) {
	BluetoothGATTCharacteristic * chr = BLUETOOTH_GATT_CHARACTERISTIC(object);
	debug("Freeing GATT characteristic: %s", chr->path_full != NULL ? chr->path_full : chr->path);
	g_free(chr->path);
	g_free(chr->path_full);
	g_strfreev(chr->flags);
	G_OBJECT_CLASS(bluetooth_gatt_characteristic_parent_class)->finalize(object);
}

static void bluetooth_gatt_characteristic_class_init(
		BluetoothGATTCharacteristicClass * _class) {
	GObjectClass * object_class = G_OBJECT_CLASS(_class);
	object_class->finalize = bluetooth_gatt_characteristic_finalize;
}

/**
 * Create new BlueZ GATT characteristic.
 *
 * @param path Export path relative to the service path.
 * @param uuid Characteristic UUID.
 * @return New BlueZ GATT characteristic. The returned object should be freed
 *   with g_object_unref(). */
BluetoothGATTCharacteristic * bluetooth_gatt_characteristic_new(
		const char * path,
		const char * uuid) {
	BluetoothGATTCharacteristic * chr = g_object_new(BLUETOOTH_TYPE_GATT_CHARACTERISTIC, NULL);
	strncpy(chr->uuid, uuid, sizeof(chr->uuid) - 1);
	chr->path = g_strdup(path);
	return chr;
}

/**
 * Set BlueZ GATT characteristic flags.
 *
 * Flags should be set before registering the GATT application.
 *
 * @param chr The BlueZ GATT characteristic.
 * @param flags NULL-terminated array of flags. */
void bluetooth_gatt_characteristic_set_flags(
		BluetoothGATTCharacteristic * chr,
		const char * const * flags) {
	chr->flags = g_strdupv((char **)flags);
}

void bluetooth_gatt_characteristic_set_read_callback(
		BluetoothGATTCharacteristic * chr,
		BluetoothGATTCharacteristicCallback callback,
		void * userdata) {
	chr->read_cb = callback;
	chr->read_cb_userdata = userdata;
}

void bluetooth_gatt_characteristic_set_acquire_notify_callback(
		BluetoothGATTCharacteristic * chr,
		BluetoothGATTCharacteristicCallback callback,
		void * userdata) {
	chr->acquire_notify_cb = callback;
	chr->acquire_notify_cb_userdata = userdata;
}

void bluetooth_gatt_characteristic_set_acquire_write_callback(
		BluetoothGATTCharacteristic * chr,
		BluetoothGATTCharacteristicCallback callback,
		void * userdata) {
	chr->acquire_write_cb = callback;
	chr->acquire_write_cb_userdata = userdata;
}
