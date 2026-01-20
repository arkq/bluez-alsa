/*
 * BlueALSA - bt-midi.c
 * SPDX-FileCopyrightText: 2023-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bt-midi.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "bluez-iface.h"
#include "bt-advertising.h"
#include "bt-gatt.h"
#include "midi.h"
#include "utils.h"
#include "shared/bluetooth.h"
#include "shared/log.h"

/**
 * Bluetooth MIDI based on BlueZ GATT application. */
struct _BluetoothMIDI {
	GObject parent;
	/* Root node of the GATT application. */
	char * path;
	/* Associated adapter. */
	struct ba_adapter * a;
	/* Associated transport. */
	struct ba_transport * t;
	/* Characteristic notify link. */
	GSource * notify_watch_hup;
	/* GATT application. */
	BluetoothGATTApplication * app;
	/* BLE advertising. */
	BluetoothAdvertising * adv;
};

G_DEFINE_TYPE(BluetoothMIDI, bluetooth_midi, G_TYPE_OBJECT)

static void bluetooth_midi_init(
		G_GNUC_UNUSED BluetoothMIDI * midi) {
}

static void bluetooth_midi_finalize(GObject * object) {
	BluetoothMIDI * midi = BLUETOOTH_MIDI(object);
	debug("Freeing BLE MIDI application: %s", midi->path);
	if (midi->notify_watch_hup != NULL) {
		g_source_destroy(midi->notify_watch_hup);
		g_source_unref(midi->notify_watch_hup);
	}
	if (midi->adv != NULL) {
		bluetooth_advertising_unregister_sync(midi->adv);
		g_object_unref(midi->adv);
	}
	g_object_unref(midi->app);
	ba_transport_destroy(midi->t);
	ba_adapter_unref(midi->a);
	g_free(midi->path);
	G_OBJECT_CLASS(bluetooth_midi_parent_class)->finalize(object);
}

static void bluetooth_midi_class_init(
		BluetoothMIDIClass * _class) {
	GObjectClass * object_class = G_OBJECT_CLASS(_class);
	object_class->finalize = bluetooth_midi_finalize;
}

/**
 * Create new local MIDI transport. */
static struct ba_transport * bluetooth_midi_transport_new(
		const BluetoothMIDI * midi) {

	struct ba_adapter * a = midi->a;
	struct ba_device * d = NULL;
	struct ba_transport * t = NULL;

	if ((d = ba_device_lookup(a, &a->hci.bdaddr)) == NULL &&
			(d = ba_device_new(a, &a->hci.bdaddr)) == NULL) {
		error("Couldn't create new device: %s", strerror(errno));
		goto fail;
	}

	if ((t = ba_transport_lookup(d, midi->path)) == NULL &&
			(t = ba_transport_new_midi(d, BA_TRANSPORT_PROFILE_MIDI, ":0", midi->path)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

fail:
	if (d != NULL)
		ba_device_unref(d);
	return t;
}

/**
 * Extract MTU from characteristic parameters. */
static uint16_t chr_get_mtu(GDBusMethodInvocation * inv) {

	GVariant * params = g_dbus_method_invocation_get_parameters(inv);
	g_autoptr(GVariant) params_ = g_variant_get_child_value(params, 0);

	uint16_t mtu;
	if (!g_variant_lookup(params_, "mtu", "q", &mtu))
		/* Fallback to minimum ATT MTU. */
		return 23;
	return mtu;
}

static bool midi_characteristic_read_value(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	GVariant * rv = g_variant_new_fixed_byte_array(NULL /* empty reply */, 0);
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(&rv, 1));
	return true;
}

/* Unfortunately, BlueZ doesn't provide any meaningful information about the
 * remote device which wants to acquire the write/notify access. There is a
 * "device" option, but the acquire-write and acquire-notify methods are called
 * only for the first device, and the application (us) is not notified when
 * some other device wants to acquire the access. Therefore, from our point of
 * view, we can tell only that there will be an incoming connection from given
 * adapter. */

static bool midi_characteristic_acquire_write(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothMIDI * midi = userdata;

	struct ba_transport * t = midi->t;
	uint16_t mtu = chr_get_mtu(inv);

	int fds[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) == -1) {
		error("Couldn't create BLE-MIDI char write socket pair: %s", strerror(errno));
		goto fail;
	}

	debug("New BLE-MIDI write link (MTU: %u): %d", mtu, fds[0]);
	t->midi.ble_fd_write = fds[0];
	t->mtu_read = mtu;

	/* TODO: Find a way to detect "device" disconnection condition. */

	midi_transport_start_watch_ble_midi(t);

	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, mtu), fd_list);
	g_object_unref(fd_list);

	return true;

fail:
	g_dbus_method_invocation_return_dbus_error(inv,
			BLUEZ_ERROR_FAILED, "Unable to acquire write access");
	return false;
}

static int midi_characteristic_release_notify(
		G_GNUC_UNUSED GIOChannel * ch,
		G_GNUC_UNUSED GIOCondition cond,
		void * userdata) {

	BluetoothMIDI * midi = userdata;
	struct ba_transport * t = midi->t;

	g_source_unref(g_steal_pointer(&midi->notify_watch_hup));

	debug("Releasing BLE-MIDI notify link: %d", t->midi.ble_fd_notify);

	close(t->midi.ble_fd_notify);
	t->midi.ble_fd_notify = -1;

	/* Remove channel from watch. */
	return G_SOURCE_REMOVE;
}

static bool midi_characteristic_acquire_notify(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothMIDI * midi = userdata;

	struct ba_transport * t = midi->t;
	uint16_t mtu = chr_get_mtu(inv);

	int fds[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) == -1) {
		error("Couldn't create BLE-MIDI char notify socket pair: %s", strerror(errno));
		goto fail;
	}

	debug("New BLE-MIDI notify link (MTU: %u): %d", mtu, fds[0]);
	t->midi.ble_fd_notify = fds[0];
	ble_midi_encode_set_mtu(&t->midi.ble_encoder, mtu);
	t->mtu_write = mtu;

	/* Setup IO watch for checking HUP condition on the socket. HUP means
	 * that the client does not want to receive notifications anymore. */
	GIOChannel * ch = g_io_channel_unix_raw_new(dup(fds[0]));
	midi->notify_watch_hup = g_io_create_watch_full(ch, G_PRIORITY_DEFAULT,
			G_IO_HUP, midi_characteristic_release_notify, midi, NULL);
	g_io_channel_unref(ch);

	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, mtu), fd_list);
	g_object_unref(fd_list);

	return true;

fail:
	g_dbus_method_invocation_return_dbus_error(inv,
			BLUEZ_ERROR_FAILED, "Unable to acquire notification");
	return false;
}

static void app_register_finish(
		GObject * source, GAsyncResult * result, void * userdata) {
	BluetoothMIDI * midi = userdata;

	g_autoptr(GError) err = NULL;
	BluetoothGATTApplication * app = BLUETOOTH_GATT_APPLICATION(source);
	if (!bluetooth_gatt_application_register_finish(app, result, &err))
		error("Couldn't register BLE-MIDI GATT application: %s", err->message);
	else if (config.midi.advertise)
		bluetooth_advertising_register(midi->adv, midi->a, NULL, NULL);

}

BluetoothMIDI * bluetooth_midi_new(
		struct ba_adapter * adapter,
		const char * path) {

	BluetoothMIDI * midi = g_object_new(BLUETOOTH_TYPE_MIDI, NULL);
	midi->a = ba_adapter_ref(adapter);
	midi->path = g_strdup(path);

	struct ba_transport * t;
	/* Setup local MIDI transport associated with our GATT server. */
	if ((t = bluetooth_midi_transport_new(midi)) == NULL)
		error("Couldn't create local MIDI transport: %s", strerror(errno));
	else if (ba_transport_acquire(t) == -1)
		error("Couldn't acquire local MIDI transport: %s", strerror(errno));
	else if (ba_transport_start(t) == -1)
		error("Couldn't start local MIDI transport: %s", strerror(errno));
	midi->t = t;

	/* Setup GATT application for BLE-MIDI. */
	midi->app = bluetooth_gatt_application_new(path);

	g_autoptr(BluetoothGATTService) srv;
	srv = bluetooth_gatt_service_new("/service", BT_UUID_MIDI, true);
	bluetooth_gatt_application_add_service(midi->app, srv);

	g_autoptr(BluetoothGATTCharacteristic) chr;
	chr = bluetooth_gatt_characteristic_new("/char", BT_UUID_MIDI_CHAR);
	bluetooth_gatt_application_add_service_characteristic(midi->app, srv, chr);

	const char * const flags[] = {
		"read", "write", "write-without-response", "notify", NULL };
	bluetooth_gatt_characteristic_set_flags(chr, flags);

	bluetooth_gatt_characteristic_set_read_callback(chr,
			midi_characteristic_read_value, midi);
	bluetooth_gatt_characteristic_set_acquire_notify_callback(chr,
			midi_characteristic_acquire_notify, midi);
	bluetooth_gatt_characteristic_set_acquire_write_callback(chr,
			midi_characteristic_acquire_write, midi);

	if (config.midi.advertise) {
		char adv_path[256];
		snprintf(adv_path, sizeof(adv_path), "%s/adv", path);
		midi->adv = bluetooth_advertising_new(
				bluetooth_gatt_application_get_object_manager_server(midi->app),
				adv_path, BT_UUID_MIDI, config.midi.name);
	}

	bluetooth_gatt_application_set_connection(midi->app, config.dbus);
	bluetooth_gatt_application_register(midi->app, adapter, app_register_finish, midi);

	return midi;
}
