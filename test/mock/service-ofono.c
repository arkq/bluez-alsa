/*
 * service-ofono.c
 * SPDX-FileCopyrightText: 2024-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "service.h"

#include <stddef.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ofono-iface.h"

#include "dbus-ifaces.h"

typedef struct OFonoMockServicePriv {
	/* Global oFono manager. */
	MockOfonoManager * manager;
	/* Global oFono HF audio manager. */
	MockOfonoHandsfreeAudioManager * hf_manager;
} OFonoMockServicePriv;

static gboolean mock_ofono_get_modems_handler(MockOfonoManager * object,
		GDBusMethodInvocation * invocation, G_GNUC_UNUSED void * userdata) {
	mock_ofono_manager_complete_get_modems(object, invocation, g_variant_new("a(oa{sv})", NULL));
	return TRUE;
}

static MockOfonoManager * manager_new(GDBusConnection * conn, const char * path) {
	MockOfonoManager * manager = mock_ofono_manager_skeleton_new();
	g_signal_connect(manager, "handle-get-modems", G_CALLBACK(mock_ofono_get_modems_handler), NULL);
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(manager), conn, path, NULL);
	return manager;
}

static gboolean mock_ofono_get_cards_handler(MockOfonoHandsfreeAudioManager * object,
		GDBusMethodInvocation * invocation, G_GNUC_UNUSED void * userdata) {
	mock_ofono_handsfree_audio_manager_complete_get_cards(object, invocation, g_variant_new("a(oa{sv})", NULL));
	return TRUE;
}

static gboolean mock_ofono_register_handler(MockOfonoHandsfreeAudioManager * object,
		GDBusMethodInvocation * invocation, G_GNUC_UNUSED const char * agent,
		G_GNUC_UNUSED GVariant * codecs, G_GNUC_UNUSED void * userdata) {
	mock_ofono_handsfree_audio_manager_complete_register(object, invocation);
	return TRUE;
}

static MockOfonoHandsfreeAudioManager * hf_audio_manager_new(GDBusConnection * conn, const char * path) {
	MockOfonoHandsfreeAudioManager * hf_manager = mock_ofono_handsfree_audio_manager_skeleton_new();
	g_signal_connect(hf_manager, "handle-get-cards", G_CALLBACK(mock_ofono_get_cards_handler), NULL);
	g_signal_connect(hf_manager, "handle-register", G_CALLBACK(mock_ofono_register_handler), NULL);
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(hf_manager), conn, path, NULL);
	return hf_manager;
}

static void name_acquired(GDBusConnection * conn,
		G_GNUC_UNUSED const char * name, void * userdata) {
	OFonoMockService * srv = userdata;
	OFonoMockServicePriv * priv = srv->priv;

	priv->manager = manager_new(conn, "/");
	priv->hf_manager = hf_audio_manager_new(conn, "/");

	mock_service_ready(&srv->service);
}

OFonoMockService * mock_ofono_service_new(void) {
	OFonoMockService * srv = g_new0(OFonoMockService, 1);
	srv->service.name = OFONO_SERVICE;
	srv->service.name_acquired_cb = name_acquired;
	srv->priv = g_new0(OFonoMockServicePriv, 1);
	return srv;
}

void mock_ofono_service_free(OFonoMockService * srv) {

	struct OFonoMockServicePriv * priv = srv->priv;
	g_object_unref(priv->manager);
	g_object_unref(priv->hf_manager);

	g_free(srv->priv);
	g_free(srv);

}
