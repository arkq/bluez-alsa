/*
 * BlueALSA - bluez-le-advertisement.c
 * SPDX-FileCopyrightText: 2023-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bluez-le-advertisement.h"

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

/**
 * BlueZ LE advertisement configuration. */
struct _BlueZLEAdvertisement {
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

G_DEFINE_TYPE(BlueZLEAdvertisement, bluez_le_advertisement, G_TYPE_OBJECT)

static void bluez_le_advertisement_init(
		G_GNUC_UNUSED BlueZLEAdvertisement * adv) {
}

static void bluez_le_advertisement_finalize(
		GObject * object) {
#if DEBUG
	BlueZLEAdvertisement * adv = BLUEZ_LE_ADVERTISEMENT(object);
	debug("Freeing LE advertisement [%s]: %s", adv->path, adv->name);
#endif
	G_OBJECT_CLASS(bluez_le_advertisement_parent_class)->finalize(object);
}

static void bluez_le_advertisement_class_init(
		G_GNUC_UNUSED BlueZLEAdvertisementClass * _class) {
	GObjectClass * object_class = G_OBJECT_CLASS(_class);
	object_class->finalize = bluez_le_advertisement_finalize;
}

static void le_advertisement_release(
		BlueZLEAdvertisement * adv) {
	ba_adapter_unref(g_steal_pointer(&adv->adapter));
	adv->registered = false;
	g_object_unref(adv);
}

static void advertisement_release(
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	BlueZLEAdvertisement * adv = userdata;
	debug("Releasing LE advertisement [%s]: %s", adv->path, adv->name);
	le_advertisement_release(adv);
	g_object_unref(inv);
}

static GVariant * advertisement_iface_get_property(
		const char * property, G_GNUC_UNUSED GError ** error, void * userdata) {
	BlueZLEAdvertisement * adv = userdata;

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
			GVariant * array = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
					adv->service_data, adv->service_data_len, sizeof(uint8_t));
			g_variant_builder_add(&builder, "{sv}", adv->uuid, array);
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
		BlueZLEAdvertisement * adv) {

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
 * Create a new BlueZ LE advertisement.
 *
 * @param manager The D-Bus object manager server to hold the advertisement.
 * @param uuid The service UUID to advertise.
 * @param name The device name to advertise.
 * @param path The D-Bus object registration path.
 * @return The LE advertisement object. The returned LE advertisement object
 *   should be freed with g_object_unref(). */
BlueZLEAdvertisement * bluez_le_advertisement_new(
		GDBusObjectManagerServer * manager,
		const char * uuid,
		const char * name,
		const char * path) {

	BlueZLEAdvertisement * adv = g_object_new(BLUEZ_TYPE_LE_ADVERTISEMENT, NULL);
	strncpy(adv->uuid, uuid, sizeof(adv->uuid) - 1);
	strncpy(adv->name, name, sizeof(adv->name) - 1);
	strncpy(adv->path, path, sizeof(adv->path) - 1);

	g_autoptr(GDBusObjectSkeleton) skeleton = advertisement_skeleton_new(adv);
	g_dbus_object_manager_server_export(manager, skeleton);

	return adv;
}

/**
 * Set the service data for the LE advertisement.
 *
 * @param adv The LE advertisement.
 * @param data The service data.
 * @param len The length of the service data.
 * @return ERROR_CODE_OK on success, otherwise an appropriate error code. */
error_code_t bluez_le_advertisement_set_service_data(
		BlueZLEAdvertisement * adv,
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
	BlueZLEAdvertisement * adv = userdata;

	g_autoptr(GDBusMessage) rep;
	g_autoptr(GError) err = NULL;
	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	g_autoptr(GTask) task;
	task = g_task_new(adv, NULL, adv->register_cb, adv->register_userdata);

	if (err != NULL) {
		error("Couldn't register LE advertisement [%s]: %s", adv->path, err->message);
		ba_adapter_unref(g_steal_pointer(&adv->adapter));
		g_task_return_error(task, g_steal_pointer(&err));
		g_object_unref(adv);
	}
	else {
		adv->registered = true;
		g_task_return_boolean(task, TRUE);
	}

}

/**
 * Register the LE advertisement on the specified adapter.
 *
 * @param adv The LE advertisement.
 * @param adapter The adapter on which to register the advertisement.
 * @param callback The registration completion callback.
 * @param userdata The user data for the registration completion callback. */
void bluez_le_advertisement_register(
		BlueZLEAdvertisement * adv,
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

	debug("Registering LE advertisement [%s]: %s", adv->path, adv->name);
	g_dbus_connection_send_message_with_reply(config.dbus, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
			register_advertisement_finish, g_object_ref(adv));

	g_object_unref(msg);

}

bool bluez_le_advertisement_register_finish(
		BlueZLEAdvertisement * adv,
		GAsyncResult * result,
		GError ** error) {
	adv->register_cb = NULL;
	adv->register_userdata = NULL;
	return g_task_propagate_boolean(G_TASK(result), error);
}

void bluez_le_advertisement_unregister_sync(
		BlueZLEAdvertisement * adv) {

	if (!adv->registered)
		return;

	g_autoptr(GDBusMessage) msg;
	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adv->adapter->bluez_dbus_path,
			BLUEZ_IFACE_LE_ADVERTISING_MANAGER, "UnregisterAdvertisement");
	g_dbus_message_set_body(msg, g_variant_new("(o)", adv->path));

	g_autoptr(GDBusMessage) rep;
	g_autoptr(GError) err = NULL;
	debug("Unregistering LE advertisement [%s]: %s", adv->path, adv->name);
	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, 1000, NULL, NULL, NULL)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	if (err != NULL)
		error("Couldn't unregister LE advertisement [%s]: %s", adv->path, err->message);
	else {
		le_advertisement_release(adv);
	}

}
