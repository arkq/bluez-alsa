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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-config.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "dbus.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rc.h"

/**
 * BlueZ LE advertisement configuration. */
struct bluez_le_advertisement {
	rc_t _rc;
	/* Service UUID. */
	char uuid[37];
	/* Device name to advertise. */
	char name[16];
	/* D-Bus object registration path. */
	char path[64];
	/* Adapter on which the advertisement is registered. */
	struct ba_adapter * adapter;
	bool registered;
};

static void le_advertisement_release(
		struct bluez_le_advertisement * adv) {
	ba_adapter_unref(g_steal_pointer(&adv->adapter));
	adv->registered = false;
	rc_unref(adv);
}

static void advertisement_release(
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	struct bluez_le_advertisement * adv = userdata;
	debug("Releasing LE advertisement [%s]: %s", adv->path, adv->name);
	le_advertisement_release(adv);
	g_object_unref(inv);
}

static GVariant * advertisement_iface_get_property(
		const char * property, G_GNUC_UNUSED GError ** error, void * userdata) {
	struct bluez_le_advertisement * adv = userdata;

	if (strcmp(property, "Type") == 0)
		return g_variant_new_string("peripheral");
	if (strcmp(property, "ServiceUUIDs") == 0) {
		const char * uuids[] = { adv->uuid };
		return g_variant_new_strv(uuids, ARRAYSIZE(uuids));
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
		struct bluez_le_advertisement * adv) {

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

	OrgBluezLeadvertisement1Skeleton * ifs_adv;
	if ((ifs_adv = org_bluez_leadvertisement1_skeleton_new(&vtable,
					adv, rc_unref)) == NULL)
		return NULL;

	rc_ref(adv);
	GDBusInterfaceSkeleton * ifs = G_DBUS_INTERFACE_SKELETON(ifs_adv);
	GDBusObjectSkeleton * skeleton = g_dbus_object_skeleton_new(adv->path);
	g_dbus_object_skeleton_add_interface(skeleton, ifs);
	g_object_unref(ifs_adv);

	return skeleton;
}

static void advertisement_free(void * ptr) {
	struct bluez_le_advertisement * adv = ptr;
	debug("Freeing LE advertisement [%s]: %s", adv->path, adv->name);
	free(adv);
}

struct bluez_le_advertisement * bluez_le_advertisement_new(
		GDBusObjectManagerServer * manager, const char * uuid, const char * name,
		const char * path) {

	struct bluez_le_advertisement * adv;
	if ((adv = calloc(1, sizeof(*adv))) == NULL)
		return NULL;

	rc_init(&adv->_rc, advertisement_free);
	strncpy(adv->uuid, uuid, sizeof(adv->uuid) - 1);
	strncpy(adv->name, name, sizeof(adv->name) - 1);
	strncpy(adv->path, path, sizeof(adv->path) - 1);

	GDBusObjectSkeleton * skeleton = advertisement_skeleton_new(adv);
	g_dbus_object_manager_server_export(manager, skeleton);
	g_object_unref(skeleton);

	return adv;
}

static void advertise_register_finish(
		GObject * source, GAsyncResult * result, void * userdata) {
	struct bluez_le_advertisement * adv = userdata;

	GDBusMessage * rep;
	GError * err = NULL;
	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) != NULL) {
		g_dbus_message_to_gerror(rep, &err);
		g_object_unref(rep);
	}

	if (err == NULL)
		adv->registered = true;
	else {
		error("Couldn't register LE advertisement [%s]: %s", adv->path, err->message);
		ba_adapter_unref(g_steal_pointer(&adv->adapter));
		g_error_free(err);
		rc_unref(adv);
	}

}

void bluez_le_advertisement_register(
		struct bluez_le_advertisement * adv, struct ba_adapter * adapter) {

	GDBusMessage * msg;
	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adapter->bluez_dbus_path,
			BLUEZ_IFACE_LE_ADVERTISING_MANAGER, "RegisterAdvertisement");
	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", adv->path, NULL));

	adv->adapter = ba_adapter_ref(adapter);
	debug("Registering LE advertisement [%s]: %s", adv->path, adv->name);
	g_dbus_connection_send_message_with_reply(config.dbus, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
			advertise_register_finish, rc_ref(adv));

	g_object_unref(msg);

}

void bluez_le_advertisement_unregister_sync(
		struct bluez_le_advertisement * adv) {

	if (!adv->registered)
		return;

	GDBusMessage * msg;
	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adv->adapter->bluez_dbus_path,
			BLUEZ_IFACE_LE_ADVERTISING_MANAGER, "UnregisterAdvertisement");
	g_dbus_message_set_body(msg, g_variant_new("(o)", adv->path));

	GDBusMessage * rep;
	GError * err = NULL;
	debug("Unregistering LE advertisement [%s]: %s", adv->path, adv->name);
	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, 1000, NULL, NULL, NULL)) != NULL) {
		g_dbus_message_to_gerror(rep, &err);
		g_object_unref(rep);
	}

	if (err == NULL)
		le_advertisement_release(adv);
	else {
		error("Couldn't unregister LE advertisement [%s]: %s", adv->path, err->message);
		g_error_free(err);
	}

	g_object_unref(msg);

}
