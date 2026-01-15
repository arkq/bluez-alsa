/*
 * BlueALSA - bt-advertising.c
 * SPDX-FileCopyrightText: 2023-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bt-advertising.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <string.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-config.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "dbus.h"
#include "error.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "utils.h"

/**
 * Bluetooth LE advertising. */
struct _BluetoothAdvertising {
	GObject parent;
	/* Service UUID. */
	char uuid[37];
	/* Device name to advertise. */
	char name[16];
	/* D-Bus object registration path. */
	char path[64];
	/* Optional service data. */
	uint8_t service_data[32];
	size_t service_data_len;
	/* Adapter on which the advertisement is registered. */
	struct ba_adapter * adapter;
	/* Registration callback and userdata. */
	GAsyncReadyCallback register_cb;
	void * register_userdata;
	bool registered;
};

G_DEFINE_TYPE(BluetoothAdvertising, bluetooth_advertising, G_TYPE_OBJECT)

static void bluetooth_advertising_init(
		G_GNUC_UNUSED BluetoothAdvertising * adv) {
}

static void bluetooth_advertising_finalize(
		GObject * object) {
	BluetoothAdvertising * adv = BLUETOOTH_ADVERTISING(object);
	debug("Freeing BLE advertising: %s", adv->path);
	if (adv->adapter != NULL)
		ba_adapter_unref(adv->adapter);
	G_OBJECT_CLASS(bluetooth_advertising_parent_class)->finalize(object);
}

static void bluetooth_advertising_class_init(
		BluetoothAdvertisingClass * _class) {
	GObjectClass * object_class = G_OBJECT_CLASS(_class);
	object_class->finalize = bluetooth_advertising_finalize;
}

static void advertising_release(
		BluetoothAdvertising * adv) {
	ba_adapter_unref(g_steal_pointer(&adv->adapter));
	adv->registered = false;
	g_object_unref(adv);
}

static void advertisement_release(
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	BluetoothAdvertising * adv = userdata;
	debug("Releasing BLE advertising [%s]: %s", adv->name, adv->path);
	advertising_release(adv);
	g_object_unref(inv);
}

static GVariant * advertisement_iface_get_property(
		const char * property, G_GNUC_UNUSED GError ** error, void * userdata) {
	const BluetoothAdvertising * adv = userdata;

	if (strcmp(property, "Type") == 0)
		return g_variant_new_string("peripheral");
	if (strcmp(property, "ServiceUUIDs") == 0) {
		const char * uuids[] = { adv->uuid };
		return g_variant_new_strv(uuids, ARRAYSIZE(uuids));
	}
	if (strcmp(property, "ServiceData") == 0) {
		GVariantBuilder builder;
		g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
		if (adv->service_data_len > 0) {
			g_variant_builder_add(&builder, "{sv}", adv->uuid,
					g_variant_new_fixed_byte_array(adv->service_data, adv->service_data_len));
		}
		return g_variant_builder_end(&builder);
	}
	if (strcmp(property, "Discoverable") == 0)
		/* Advertise as general discoverable LE-only device. */
		return g_variant_new_boolean(TRUE);
	if (strcmp(property, "LocalName") == 0)
		return g_variant_new_string(adv->name);

	g_assert_not_reached();
	return NULL;
}

static GDBusObjectSkeleton * advertisement_skeleton_new(
		BluetoothAdvertising * adv) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "Release",
			.sender = bluez_dbus_unique_name,
			.handler = advertisement_release },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
		.get_property = advertisement_iface_get_property,
	};

	GDBusObjectSkeleton * skeleton = g_dbus_object_skeleton_new(adv->path);

	g_autoptr(OrgBluezLeadvertisement1Skeleton) ifs;
	ifs = org_bluez_leadvertisement1_skeleton_new(&vtable, g_object_ref(adv), g_object_unref);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(ifs));

	return skeleton;
}

/**
 * Create a new BLE advertising.
 *
 * @param manager The D-Bus object manager server to hold the advertising.
 * @param uuid The service UUID to advertise.
 * @param name The device name to advertise.
 * @param path The D-Bus object registration path.
 * @return The BLE advertising object. The returned BLE advertising object
 *   should be freed with g_object_unref(). */
BluetoothAdvertising * bluetooth_advertising_new(
		GDBusObjectManagerServer * manager,
		const char * path,
		const char * uuid,
		const char * name) {

	BluetoothAdvertising * adv = g_object_new(BLUETOOTH_TYPE_ADVERTISING, NULL);
	strncpy(adv->path, path, sizeof(adv->path) - 1);
	strncpy(adv->uuid, uuid, sizeof(adv->uuid) - 1);
	strncpy(adv->name, name, sizeof(adv->name) - 1);

	g_autoptr(GDBusObjectSkeleton) skeleton = advertisement_skeleton_new(adv);
	g_dbus_object_manager_server_export(manager, skeleton);

	return adv;
}

/**
 * Set the service data for the BLE advertising.
 *
 * @param adv The BLE advertising.
 * @param data The service data.
 * @param len The length of the service data.
 * @return ERROR_CODE_OK on success, otherwise an appropriate error code. */
error_code_t bluetooth_advertising_set_service_data(
		BluetoothAdvertising * adv,
		const uint8_t * data,
		size_t len) {

	if (len > sizeof(adv->service_data))
		return ERROR_CODE_INVALID_SIZE;

	memcpy(adv->service_data, data, len);
	adv->service_data_len = len;

	return ERROR_CODE_OK;
}

static void register_advertisement_finish(
		GObject * source, GAsyncResult * result, void * userdata) {
	BluetoothAdvertising * adv = userdata;

	g_autoptr(GDBusMessage) rep;
	g_autoptr(GError) err = NULL;
	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	g_autoptr(GTask) task;
	task = g_task_new(adv, NULL, adv->register_cb, adv->register_userdata);

	if (err != NULL) {
		error("Couldn't register BLE advertisement [%s]: %s", adv->name, err->message);
		ba_adapter_unref(g_steal_pointer(&adv->adapter));
		g_task_return_error(task, g_steal_pointer(&err));
		g_object_unref(adv);
	}
	else {
		adv->registered = true;
		/* NOTE: The reference to the Bluetooth advertising object shall be held
		 *       as long as the advertisement is registered in BlueZ. */
		g_task_return_boolean(task, TRUE);
	}

}

/**
 * Register the LE advertisement on the specified adapter.
 *
 * @param adv The BLE advertising.
 * @param adapter The adapter on which to register the advertisement.
 * @param callback The registration completion callback.
 * @param userdata The user data for the registration completion callback. */
void bluetooth_advertising_register(
		BluetoothAdvertising * adv,
		struct ba_adapter * adapter,
		GAsyncReadyCallback callback,
		void * userdata) {

	GDBusMessage * msg;
	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adapter->bluez_dbus_path,
			BLUEZ_IFACE_LE_ADVERTISING_MANAGER, "RegisterAdvertisement");
	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", adv->path, NULL));

	adv->adapter = ba_adapter_ref(adapter);
	adv->register_cb = callback;
	adv->register_userdata = userdata;

	debug("Registering BLE advertisement [%s]: %s", adv->name, adv->path);
	g_dbus_connection_send_message_with_reply(config.dbus, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
			register_advertisement_finish, g_object_ref(adv));

	g_object_unref(msg);

}

bool bluetooth_advertising_register_finish(
		BluetoothAdvertising * adv,
		GAsyncResult * result,
		GError ** error) {
	adv->register_cb = NULL;
	adv->register_userdata = NULL;
	return g_task_propagate_boolean(G_TASK(result), error);
}

void bluetooth_advertising_unregister_sync(
		BluetoothAdvertising * adv) {

	if (!adv->registered)
		return;

	g_autoptr(GDBusMessage) msg;
	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adv->adapter->bluez_dbus_path,
			BLUEZ_IFACE_LE_ADVERTISING_MANAGER, "UnregisterAdvertisement");
	g_dbus_message_set_body(msg, g_variant_new("(o)", adv->path));

	g_autoptr(GDBusMessage) rep;
	g_autoptr(GError) err = NULL;
	debug("Unregistering BLE advertisement [%s]: %s", adv->name, adv->path);
	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, 1000, NULL, NULL, NULL)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	if (err != NULL)
		error("Couldn't unregister BLE advertisement [%s]: %s", adv->path, err->message);
	else {
		advertising_release(adv);
	}

}
