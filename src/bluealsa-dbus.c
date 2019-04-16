/*
 * BlueALSA - bluealsa-dbus.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-dbus.h"

#include <string.h>

#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-device.h"
#include "bluealsa-iface.h"
#include "bluealsa.h"
#include "shared/log.h"

static GVariant *ba_variant_new_device_path(const struct ba_transport *t) {
	return g_variant_new_object_path(t->d->bluez_dbus_path);
}

static GVariant *ba_variant_new_pcm_modes(const struct ba_transport *t) {
	static const char *modes[] = {
		BLUEALSA_PCM_MODE_SOURCE, BLUEALSA_PCM_MODE_SINK };
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return g_variant_new_strv(&modes[0], 1);
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return g_variant_new_strv(&modes[1], 1);
	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		return g_variant_new_strv(modes, 2);
	return g_variant_new_strv(modes, 0);
}

static GVariant *ba_variant_new_channels(const struct ba_transport *t) {
	return g_variant_new_byte(ba_transport_get_channels(t));
}

static GVariant *ba_variant_new_sampling(const struct ba_transport *t) {
	return g_variant_new_uint32(ba_transport_get_sampling(t));
}

static void bluealsa_manager_get_pcms(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GVariantBuilder pcms;
	g_variant_builder_init(&pcms, G_VARIANT_TYPE("a{oa{sv}}"));

	struct ba_adapter *a;
	size_t i;

	for (i = 0; i < HCI_MAX_DEV; i++) {

		if ((a = ba_adapter_lookup(i)) == NULL)
			continue;

		GHashTableIter iter_d, iter_t;
		struct ba_device *d;
		struct ba_transport *t;

		g_hash_table_iter_init(&iter_d, a->devices);
		while (g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d)) {
			g_hash_table_iter_init(&iter_t, d->transports);
			while (g_hash_table_iter_next(&iter_t, NULL, (gpointer)&t)) {

				if (t->type.profile & BA_TRANSPORT_PROFILE_RFCOMM)
					continue;

				GVariantBuilder props;
				g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
				g_variant_builder_add(&props, "{sv}", "Device", ba_variant_new_device_path(t));
				g_variant_builder_add(&props, "{sv}", "Modes", ba_variant_new_pcm_modes(t));
				g_variant_builder_add(&props, "{sv}", "Channels", ba_variant_new_channels(t));
				g_variant_builder_add(&props, "{sv}", "Sampling", ba_variant_new_sampling(t));

				g_variant_builder_add(&pcms, "{oa{sv}}", t->ba_dbus_path, &props);
				g_variant_builder_clear(&props);
			}
		}

	}

	g_dbus_method_invocation_return_value(inv, g_variant_new("(a{oa{sv}})", &pcms));
	g_variant_builder_clear(&pcms);
}

static void bluealsa_manager_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	debug("Called: %s.%s()", interface, method);
	(void)conn;
	(void)sender;
	(void)path;
	(void)params;

	if (strcmp(method, "GetPCMs") == 0)
		bluealsa_manager_get_pcms(invocation, userdata);

}

/**
 * Register BlueALSA D-Bus manager interface. */
int bluealsa_dbus_register_manager(GError **error) {
	static const GDBusInterfaceVTable vtable = {
		.method_call = bluealsa_manager_method_call };
	return g_dbus_connection_register_object(config.dbus, "/org/bluealsa",
			(GDBusInterfaceInfo *)&bluealsa_iface_manager, &vtable, NULL, NULL, error);
}

static GVariant *bluealsa_transport_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *interface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;

	struct ba_transport *t = (struct ba_transport *)userdata;

	if (strcmp(property, "Device") == 0)
		return ba_variant_new_device_path(t);
	if (strcmp(property, "Modes") == 0)
		return ba_variant_new_pcm_modes(t);
	if (strcmp(property, "Channels") == 0)
		return ba_variant_new_channels(t);
	if (strcmp(property, "Sampling") == 0)
		return ba_variant_new_sampling(t);

	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"No such property '%s'", property);
	return NULL;
}

/**
 * Register BlueALSA D-Bus transport (PCM) interface. */
int bluealsa_dbus_register_transport(struct ba_transport *t) {

	static const GDBusInterfaceVTable vtable = {
		.get_property = bluealsa_transport_get_property,
	};

	t->ba_dbus_id = g_dbus_connection_register_object(config.dbus,
			t->ba_dbus_path, (GDBusInterfaceInfo *)&bluealsa_iface_pcm,
			&vtable, t, NULL, NULL);

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&props, "{sv}", "Device", ba_variant_new_device_path(t));
	g_variant_builder_add(&props, "{sv}", "Modes", ba_variant_new_pcm_modes(t));
	g_variant_builder_add(&props, "{sv}", "Channels", ba_variant_new_channels(t));
	g_variant_builder_add(&props, "{sv}", "Sampling", ba_variant_new_sampling(t));

	g_dbus_connection_emit_signal(config.dbus, NULL,
			"/org/bluealsa", BLUEALSA_IFACE_MANAGER, "PCMAdded",
			g_variant_new("(oa{sv})", t->ba_dbus_path, &props), NULL);
	g_variant_builder_clear(&props);

	return t->ba_dbus_id;
}

void bluealsa_dbus_unregister_transport(struct ba_transport *t) {
	if (t->ba_dbus_id != 0) {
		g_dbus_connection_unregister_object(config.dbus, t->ba_dbus_id);
		g_dbus_connection_emit_signal(config.dbus, NULL,
				"/org/bluealsa", BLUEALSA_IFACE_MANAGER, "PCMRemoved",
				g_variant_new("(o)", t->ba_dbus_path), NULL);
	}
}
