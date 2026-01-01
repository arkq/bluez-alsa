/*
 * BlueALSA - bluez-midi.c
 * SPDX-FileCopyrightText: 2023-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bluez-midi.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include <bluetooth/bluetooth.h> /* IWYU pragma: keep */
#include <bluetooth/hci.h>

#include "ba-adapter.h"
#include "ba-config.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "ble-midi.h"
#include "bluez.h"
#include "bluez-iface.h"
#include "bluez-le-advertisement.h"
#include "dbus.h"
#include "midi.h"
#include "utils.h"
#include "shared/bluetooth.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rc.h"

/**
 * BlueALSA MIDI GATT application. */
struct bluez_midi_app {
	rc_t _rc;
	/* D-Bus object registration paths. */
	char path[64];
	char path_service[64 + 8];
	char path_char[64 + 16];
	/* Associated adapter. */
	int hci_dev_id;
	/* Associated transport. */
	struct ba_transport * t;
	/* Characteristic write link. */
	bool write_acquired;
	/* Characteristic notify link. */
	GSource * notify_watch_hup;
	bool notify_acquired;
	/* LE advertisement. */
	struct bluez_le_advertisement * adv;
};

/**
 * Create new local MIDI transport. */
static struct ba_transport *bluez_midi_transport_new(
		struct bluez_midi_app *app) {

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	if ((a = ba_adapter_lookup(app->hci_dev_id)) == NULL) {
		error("Couldn't lookup adapter: hci%d: %s", app->hci_dev_id, strerror(errno));
		goto fail;
	}

	if ((d = ba_device_lookup(a, &a->hci.bdaddr)) == NULL &&
			(d = ba_device_new(a, &a->hci.bdaddr)) == NULL) {
		error("Couldn't create new device: %s", strerror(errno));
		goto fail;
	}

	if ((t = ba_transport_lookup(d, app->path)) == NULL &&
			(t = ba_transport_new_midi(d, BA_TRANSPORT_PROFILE_MIDI, ":0", app->path)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

fail:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	return t;
}

static GVariant *bluez_midi_service_iface_get_property(
		const char *property, G_GNUC_UNUSED GError **error,
		G_GNUC_UNUSED void *userdata) {

	if (strcmp(property, "UUID") == 0)
		return g_variant_new_string(BT_UUID_MIDI);
	if (strcmp(property, "Primary") == 0)
		return g_variant_new_boolean(TRUE);

	g_assert_not_reached();
	return NULL;
}

static GDBusObjectSkeleton * bluez_midi_service_skeleton_new(
		struct bluez_midi_app * app) {

	static const GDBusInterfaceSkeletonVTable vtable = {
		.get_property = bluez_midi_service_iface_get_property,
	};

	OrgBluezGattService1Skeleton * ifs_gatt_service;
	if ((ifs_gatt_service = org_bluez_gatt_service1_skeleton_new(&vtable,
					app, rc_unref)) == NULL)
		return NULL;

	rc_ref(app);
	GDBusInterfaceSkeleton * ifs = G_DBUS_INTERFACE_SKELETON(ifs_gatt_service);
	GDBusObjectSkeleton * skeleton = g_dbus_object_skeleton_new(app->path_service);
	g_dbus_object_skeleton_add_interface(skeleton, ifs);
	g_object_unref(ifs_gatt_service);

	return skeleton;
}

static void bluez_midi_characteristic_read_value(
		GDBusMethodInvocation *inv, G_GNUC_UNUSED void *userdata) {
	GVariant *rv[] = { /* respond with no payload */
		g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, NULL, 0, sizeof(uint8_t)) };
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(rv, 1));
}

static bool bluez_midi_params_get_mtu(GVariant *params, uint16_t *mtu) {
	GVariant *params_ = g_variant_get_child_value(params, 0);
	bool ok = g_variant_lookup(params_, "mtu", "q", mtu) == TRUE;
	g_variant_unref(params_);
	return ok;
}

/* Unfortunately, BlueZ doesn't provide any meaningful information about the
 * remote device which wants to acquire the write/notify access. There is a
 * "device" option, but the acquire-write and acquire-notify methods are called
 * only for the first device, and the application (us) is not notified when
 * some other device wants to acquire the access. Therefore, from our point of
 * view, we can tell only that there will be an incoming connection from given
 * adapter. */

static void bluez_midi_characteristic_acquire_write(
		GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_midi_app *app = userdata;
	struct ba_transport *t = app->t;

	uint16_t mtu = 0;
	if (!bluez_midi_params_get_mtu(params, &mtu)) {
		error("Couldn't acquire BLE-MIDI char write: %s", "Invalid options");
		goto fail;
	}

	int fds[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) == -1) {
		error("Couldn't create BLE-MIDI char write socket pair: %s", strerror(errno));
		goto fail;
	}

	debug("New BLE-MIDI write link (MTU: %u): %d", mtu, fds[0]);
	app->write_acquired = true;
	t->midi.ble_fd_write = fds[0];
	t->mtu_read = mtu;

	/* TODO: Find a way to detect "device" disconnection condition. */

	midi_transport_start_watch_ble_midi(t);

	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, mtu), fd_list);
	g_object_unref(fd_list);

	return;

fail:
	g_dbus_method_invocation_return_dbus_error(inv,
			BLUEZ_ERROR_FAILED, "Unable to acquire write access");
}

static gboolean bluez_midi_characteristic_release_notify(
		G_GNUC_UNUSED GIOChannel * ch,
		G_GNUC_UNUSED GIOCondition cond,
		void * userdata) {

	struct bluez_midi_app * app = userdata;
	struct ba_transport * t = app->t;

	g_source_unref(g_steal_pointer(&app->notify_watch_hup));

	debug("Releasing BLE-MIDI notify link: %d", t->midi.ble_fd_notify);

	app->notify_acquired = false;
	close(t->midi.ble_fd_notify);
	t->midi.ble_fd_notify = -1;

	/* Remove channel from watch. */
	return G_SOURCE_REMOVE;
}

static void bluez_midi_characteristic_acquire_notify(
		GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_midi_app *app = userdata;
	struct ba_transport *t = app->t;

	uint16_t mtu = 0;
	if (!bluez_midi_params_get_mtu(params, &mtu)) {
		error("Couldn't acquire BLE-MIDI char notify: %s", "Invalid options");
		goto fail;
	}

	int fds[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) == -1) {
		error("Couldn't create BLE-MIDI char notify socket pair: %s", strerror(errno));
		goto fail;
	}

	debug("New BLE-MIDI notify link (MTU: %u): %d", mtu, fds[0]);
	app->notify_acquired = true;
	t->midi.ble_fd_notify = fds[0];
	ble_midi_encode_set_mtu(&t->midi.ble_encoder, mtu);
	t->mtu_write = mtu;

	/* Setup IO watch for checking HUP condition on the socket. HUP means
	 * that the client does not want to receive notifications anymore. */
	GIOChannel *ch = g_io_channel_unix_new(fds[0]);
	app->notify_watch_hup = g_io_create_watch_full(ch, G_PRIORITY_DEFAULT,
			G_IO_HUP, bluez_midi_characteristic_release_notify, app, NULL);
	g_io_channel_unref(ch);

	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, mtu), fd_list);
	g_object_unref(fd_list);

	return;

fail:
	g_dbus_method_invocation_return_dbus_error(inv,
			BLUEZ_ERROR_FAILED, "Unable to acquire notification");
}

static GVariant *bluez_midi_characteristic_iface_get_property(
		const char *property, G_GNUC_UNUSED GError **error, void *userdata) {

	struct bluez_midi_app *app = userdata;

	if (strcmp(property, "UUID") == 0)
		return g_variant_new_string(BT_UUID_MIDI_CHAR);
	if (strcmp(property, "Service") == 0)
		return g_variant_new_object_path(app->path_service);
	if (strcmp(property, "WriteAcquired") == 0)
		return g_variant_new_boolean(app->write_acquired);
	if (strcmp(property, "NotifyAcquired") == 0)
		return g_variant_new_boolean(app->notify_acquired);
	if (strcmp(property, "Flags") == 0) {
		const char *values[] = {
			"read", "write", "write-without-response", "notify" };
		return g_variant_new_strv(values, ARRAYSIZE(values));
	}

	g_assert_not_reached();
	return NULL;
}

static GDBusObjectSkeleton * bluez_midi_characteristic_skeleton_new(
		struct bluez_midi_app * app) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "ReadValue",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_midi_characteristic_read_value },
		{ .method = "AcquireWrite",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_midi_characteristic_acquire_write },
		{ .method = "AcquireNotify",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_midi_characteristic_acquire_notify },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
		.get_property = bluez_midi_characteristic_iface_get_property,
	};

	OrgBluezGattCharacteristic1Skeleton * ifs_gatt_char;
	if ((ifs_gatt_char = org_bluez_gatt_characteristic1_skeleton_new(&vtable,
					app, rc_unref)) == NULL)
		return NULL;

	rc_ref(app);
	GDBusInterfaceSkeleton * ifs = G_DBUS_INTERFACE_SKELETON(ifs_gatt_char);
	GDBusObjectSkeleton * skeleton = g_dbus_object_skeleton_new(app->path_char);
	g_dbus_object_skeleton_add_interface(skeleton, ifs);
	g_object_unref(ifs_gatt_char);

	return skeleton;
}

static void midi_app_register_finish(
		GObject * source, GAsyncResult * result, G_GNUC_UNUSED void * userdata) {

	GDBusMessage * rep;
	GError * err = NULL;
	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't register MIDI GATT application: %s", err->message);
		g_error_free(err);
	}

}

static void bluez_midi_app_register(
		struct ba_adapter *adapter, struct bluez_midi_app *app) {

	GDBusMessage *msg;
	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adapter->bluez_dbus_path,
			BLUEZ_IFACE_GATT_MANAGER, "RegisterApplication");

	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", app->path, NULL));

	debug("Registering MIDI GATT application: %s", app->path);
	g_dbus_connection_send_message_with_reply(config.dbus, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
			midi_app_register_finish, NULL);

	g_object_unref(msg);
}

static void app_free(void * ptr) {
	struct bluez_midi_app * app = ptr;
	debug("Freeing MIDI GATT application: %s", app->path);
	if (app->notify_watch_hup != NULL) {
		g_source_destroy(app->notify_watch_hup);
		g_source_unref(app->notify_watch_hup);
	}
	bluez_le_advertisement_unregister_sync(app->adv);
	ba_transport_destroy(app->t);
	rc_unref(app->adv);
	free(app);
}

GDBusObjectManagerServer * bluez_midi_app_new(
		struct ba_adapter * adapter,
		const char * path) {

	struct bluez_midi_app * app;
	if ((app = calloc(1, sizeof(*app))) == NULL)
		return NULL;

	rc_init(&app->_rc, app_free);
	snprintf(app->path, sizeof(app->path), "%s", path);
	snprintf(app->path_service, sizeof(app->path_service), "%s/service", path);
	snprintf(app->path_char, sizeof(app->path_char), "%s/char", app->path_service);
	app->hci_dev_id = adapter->hci.dev_id;

	struct ba_transport * t;
	/* Setup local MIDI transport associated with our GATT server. */
	if ((t = bluez_midi_transport_new(app)) == NULL)
		error("Couldn't create local MIDI transport: %s", strerror(errno));
	else if (ba_transport_acquire(t) == -1)
		error("Couldn't acquire local MIDI transport: %s", strerror(errno));
	else if (ba_transport_start(t) == -1)
		error("Couldn't start local MIDI transport: %s", strerror(errno));
	app->t = t;

	GDBusObjectManagerServer * manager = g_dbus_object_manager_server_new(path);
	GDBusObjectSkeleton * skeleton;

	skeleton = bluez_midi_service_skeleton_new(app);
	g_dbus_object_manager_server_export(manager, skeleton);
	g_object_unref(skeleton);

	skeleton = bluez_midi_characteristic_skeleton_new(app);
	g_dbus_object_manager_server_export(manager, skeleton);
	g_object_unref(skeleton);

	char path_adv[sizeof(app->path) + 4];
	snprintf(path_adv, sizeof(path_adv), "%s/adv", app->path);
	app->adv = bluez_le_advertisement_new(manager, BT_UUID_MIDI,
			config.midi.name, path_adv);

	g_dbus_object_manager_server_set_connection(manager, config.dbus);

	bluez_midi_app_register(adapter, app);

	if (config.midi.advertise)
		bluez_le_advertisement_register(app->adv, adapter);

	/* The application is referenced by interface skeletons which are owned
	 * by the object manager server. Freeing the manager will free the app. */
	rc_unref(app);

	return manager;
}
