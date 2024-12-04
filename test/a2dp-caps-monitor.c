/*
 * a2dp-caps-scanner.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib-unix.h>
#include <glib.h>

#include "bluez-iface.h"
#include "shared/a2dp-codecs.h"
#include "shared/hex.h"

static void sep_added_cb(GDBusProxy * sep, GHashTable * devices) {

	g_autoptr(GVariant) v_sep_device_path = g_dbus_proxy_get_cached_property(sep, "Device");
	GDBusProxy * device = g_hash_table_lookup(devices, g_variant_get_string(v_sep_device_path, NULL));
	g_autoptr(GVariant) v_dev_address = g_dbus_proxy_get_cached_property(device, "Address");
	g_autoptr(GVariant) v_dev_name = g_dbus_proxy_get_cached_property(device, "Alias");

	g_autoptr(GVariant) v_sep_uuid = g_dbus_proxy_get_cached_property(sep, "UUID");
	g_autoptr(GVariant) v_sep_codec = g_dbus_proxy_get_cached_property(sep, "Codec");
	g_autoptr(GVariant) v_sep_caps = g_dbus_proxy_get_cached_property(sep, "Capabilities");

	const char * dev_address = g_variant_get_string(v_dev_address, NULL);
	const char * dev_name = g_variant_get_string(v_dev_name, NULL);
	const char * sep_uuid = g_variant_get_string(v_sep_uuid, NULL);

	size_t caps_size = 0;
	const uint8_t * caps_blob = g_variant_get_fixed_array(v_sep_caps, &caps_size, sizeof(uint8_t));

	uint32_t extended_codec_id;
	const uint8_t codec_id = g_variant_get_byte(v_sep_codec);
	if ((extended_codec_id = codec_id) == A2DP_CODEC_VENDOR) {
		const a2dp_vendor_info_t * info = (const a2dp_vendor_info_t *)caps_blob;
		extended_codec_id = info->vendor_id << 16 | info->codec_id;
	}

	const char * codec_name = a2dp_codecs_codec_id_to_string(extended_codec_id);
	g_autofree char * codec_name_norm = g_ascii_strdown(codec_name != NULL ? codec_name : "", -1);
	g_autofree char * caps_hex = g_malloc0((caps_size * 2) + 1);
	bin2hex(caps_blob, caps_hex, caps_size);

	const char * colon = codec_name != NULL ? ":" : "";
	g_autofree char * codec_caps_hex = g_strdup_printf("%s%s%s", codec_name_norm, colon, caps_hex);
	g_print("%s [%s]: %s: %02x: %s\n", dev_address, dev_name, sep_uuid, codec_id, codec_caps_hex);

}

static void object_added_cb(G_GNUC_UNUSED GDBusObjectManager * manager, GDBusObject * object, GHashTable * devices) {
	g_autoptr(GDBusInterface) device;
	if ((device = g_dbus_object_get_interface(object, BLUEZ_IFACE_DEVICE)) != NULL) {
		g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
				G_DBUS_PROXY_FLAGS_NONE, g_dbus_interface_get_info(device), BLUEZ_SERVICE,
				g_dbus_object_get_object_path(object), BLUEZ_IFACE_DEVICE, NULL, NULL);
		g_hash_table_insert(devices, g_strdup(g_dbus_object_get_object_path(object)), g_object_ref(proxy));
	}
	g_autoptr(GDBusInterface) endpoint;
	if ((endpoint = g_dbus_object_get_interface(object, BLUEZ_IFACE_MEDIA_ENDPOINT)) != NULL) {
		g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
				G_DBUS_PROXY_FLAGS_NONE, g_dbus_interface_get_info(endpoint), BLUEZ_SERVICE,
				g_dbus_object_get_object_path(object), BLUEZ_IFACE_MEDIA_ENDPOINT, NULL, NULL);
		sep_added_cb(proxy, devices);
	}
}

static void populate_device_objects(GDBusObjectManager * manager, GHashTable * devices) {

	GList * objects = g_dbus_object_manager_get_objects(manager);
	for (GList * l = objects; l != NULL; l = l->next) {
		g_autoptr(GDBusInterface) device;
		if ((device = g_dbus_object_get_interface(l->data, BLUEZ_IFACE_DEVICE)) != NULL)
			object_added_cb(manager, l->data, devices);
	}

	g_list_free_full(g_steal_pointer(&objects), g_object_unref);
}

static int main_loop_quit_cb(void * userdata) {
	g_main_loop_quit(userdata);
	return G_SOURCE_REMOVE;
}

int main(G_GNUC_UNUSED int argc, G_GNUC_UNUSED char * argv[]) {

	g_autoptr(GError) error = NULL;
	g_autoptr(GDBusObjectManager) manager = NULL;
	if ((manager = g_dbus_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
				G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, BLUEZ_SERVICE, "/",
				NULL, NULL, NULL, NULL, &error)) == NULL) {
		g_printerr("error: Couldn't create D-Bus object manager: %s\n", error->message);
		return 1;
	}

	g_autoptr(GHashTable) devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	g_signal_connect(manager, "object-added", G_CALLBACK(object_added_cb), devices);
	populate_device_objects(manager, devices);

	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, main_loop_quit_cb, loop);
	g_main_loop_run(loop);

	return 0;
}
