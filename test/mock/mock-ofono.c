/*
 * mock-ofono.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "mock.h"

#include <stddef.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ofono-iface.h"

#include "dbus-ifaces.h"

/* Global oFono mock manager. */
static MockOfonoManager *manager = NULL;
/* Global oFono mock HF audio manager. */
static MockOfonoHandsfreeAudioManager *hf_manager = NULL;

/* D-Bus client for registered HF agent. */
static char *hf_agent_client = NULL;
/* D-Bus object path for registered HF agent. */
static char *hf_agent_path = NULL;

static gboolean mock_ofono_get_modems_handler(MockOfonoManager *object,
		GDBusMethodInvocation *invocation, G_GNUC_UNUSED void *userdata) {
	mock_ofono_manager_complete_get_modems(object, invocation, g_variant_new("a(oa{sv})", NULL));
	return TRUE;
}

static void mock_ofono_manager_add(GDBusConnection *conn, const char *path) {
	manager = mock_ofono_manager_skeleton_new();
	g_signal_connect(manager, "handle-get-modems", G_CALLBACK(mock_ofono_get_modems_handler), NULL);
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(manager), conn, path, NULL);
}

static gboolean mock_ofono_get_cards_handler(MockOfonoHandsfreeAudioManager *object,
		GDBusMethodInvocation *invocation, G_GNUC_UNUSED void *userdata) {
	mock_ofono_handsfree_audio_manager_complete_get_cards(object, invocation, g_variant_new("a(oa{sv})", NULL));
	return TRUE;
}

static gboolean mock_ofono_register_handler(MockOfonoHandsfreeAudioManager *object,
		GDBusMethodInvocation *invocation, const char *agent, G_GNUC_UNUSED GVariant *codecs,
		G_GNUC_UNUSED void *userdata) {
	hf_agent_client = g_strdup(g_dbus_method_invocation_get_sender(invocation));
	hf_agent_path = g_strdup(agent);
	mock_ofono_handsfree_audio_manager_complete_register(object, invocation);
	return TRUE;
}

static void mock_ofono_hf_manager_add(GDBusConnection *conn, const char *path) {
	hf_manager = mock_ofono_handsfree_audio_manager_skeleton_new();
	g_signal_connect(hf_manager, "handle-get-cards", G_CALLBACK(mock_ofono_get_cards_handler), NULL);
	g_signal_connect(hf_manager, "handle-register", G_CALLBACK(mock_ofono_register_handler), NULL);
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(hf_manager), conn, path, NULL);
}

static void mock_dbus_name_acquired(GDBusConnection *conn,
		G_GNUC_UNUSED const char *name, void *userdata) {
	struct MockService *service = userdata;

	mock_ofono_manager_add(conn, "/");
	mock_ofono_hf_manager_add(conn, "/");

	mock_sem_signal(service->ready);

}

static struct MockService service = {
	.name = OFONO_SERVICE,
	.name_acquired_cb = mock_dbus_name_acquired,
};

void mock_ofono_service_start(void) {
	mock_service_start(&service);
}

void mock_ofono_service_stop(void) {

	mock_service_stop(&service);

	g_object_unref(manager);
	g_object_unref(hf_manager);
	g_free(hf_agent_client);
	g_free(hf_agent_path);

}
