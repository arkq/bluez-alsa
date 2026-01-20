/*
 * BlueALSA - bt-asha.c
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bt-asha.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
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

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/l2cap.h>

#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "ba-config.h"
#include "bluez-iface.h"
#include "bt-advertising.h"
#include "bt-gatt.h"
#include "error.h"
#include "shared/bluetooth.h"
#include "shared/bluetooth-asha.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "utils.h"

/**
 * Bluetooth ASHA server based on BlueZ GATT application. */
struct _BluetoothASHA {
	GObject parent;
	/* Root node of the GATT application. */
	char * path;
	/* Associated adapter. */
	struct ba_adapter * a;
	/* Watch for incoming ASHA connections. */
	GSource * dispatcher;
	/* Dynamically assigned PSM for connection dispatcher. */
	uint16_t psm;
	/* Transport for the current ASHA connection. */
	struct ba_transport * t;
	/* GATT application. */
	BluetoothGATTApplication * app;
	/* Watch for control point characteristic. */
	GSource * chr_ctrl_write_watch;
	/* Watch for volume characteristic. */
	GSource * chr_volume_write_watch;
	/* Notification channel for status characteristic. */
	GIOChannel * chr_status_notify_channel;
	GSource * chr_status_notify_watch_hup;
	/* Audio status point characteristic value. */
	int8_t status;
	/* BLE advertising. */
	BluetoothAdvertising * adv;
};

G_DEFINE_TYPE(BluetoothASHA, bluetooth_asha, G_TYPE_OBJECT)

static void bluetooth_asha_init(
		G_GNUC_UNUSED BluetoothASHA * asha) {
}

static void bluetooth_asha_finalize(GObject * object) {
	BluetoothASHA * asha = BLUETOOTH_ASHA(object);
	debug("Freeing BLE ASHA application: %s", asha->path);
	if (asha->dispatcher != NULL) {
		g_source_destroy(asha->dispatcher);
		g_source_unref(asha->dispatcher);
	}
	if (asha->chr_ctrl_write_watch != NULL) {
		g_source_destroy(asha->chr_ctrl_write_watch);
		g_source_unref(asha->chr_ctrl_write_watch);
	}
	if (asha->chr_volume_write_watch != NULL) {
		g_source_destroy(asha->chr_volume_write_watch);
		g_source_unref(asha->chr_volume_write_watch);
	}
	if (asha->chr_status_notify_channel != NULL)
		g_io_channel_unref(asha->chr_status_notify_channel);
	if (asha->chr_status_notify_watch_hup != NULL) {
		g_source_destroy(asha->chr_status_notify_watch_hup);
		g_source_unref(asha->chr_status_notify_watch_hup);
	}
	if (asha->adv != NULL) {
		bluetooth_advertising_unregister_sync(asha->adv);
		g_object_unref(asha->adv);
	}
	g_object_unref(asha->app);
	if (asha->t != NULL)
		ba_transport_unref(asha->t);
	ba_adapter_unref(asha->a);
	g_free(asha->path);
	G_OBJECT_CLASS(bluetooth_asha_parent_class)->finalize(object);
}

static void bluetooth_asha_class_init(
		BluetoothASHAClass * _class) {
	GObjectClass * object_class = G_OBJECT_CLASS(_class);
	object_class->finalize = bluetooth_asha_finalize;
}

static int asha_connection_dispatcher(
		GIOChannel * ch,
		G_GNUC_UNUSED GIOCondition cond,
		void * userdata) {
	BluetoothASHA * asha = userdata;

	int listen_fd = g_io_channel_unix_get_fd(ch);
	struct ba_adapter * a = asha->a;
	struct ba_device * d = NULL;
	struct ba_transport * t = NULL;
	int fd = -1;

	struct sockaddr_l2 addr = { 0 };
	int mtu_rx = 0, mtu_tx = 0;
	socklen_t len;

	len = sizeof(addr);
	if ((fd = accept(listen_fd, (struct sockaddr *)&addr, &len)) == -1) {
		error("Couldn't accept incoming ASHA connection: %s", strerror(errno));
		goto cleanup;
	}

	char addrstr[18];
	ba2str(&addr.l2_bdaddr, addrstr);
	debug("New incoming ASHA connection [%s]: %d", addrstr, fd);

	len = sizeof(mtu_rx);
	if (getsockopt(fd, SOL_BLUETOOTH, BT_RCVMTU, &mtu_rx, &len) == -1) {
		error("Couldn't get RX MTU for ASHA connection: %s", strerror(errno));
		goto cleanup;
	}

	len = sizeof(mtu_tx);
	if (getsockopt(fd, SOL_BLUETOOTH, BT_SNDMTU, &mtu_tx, &len) == -1) {
		error("Couldn't get TX MTU for ASHA connection: %s", strerror(errno));
		goto cleanup;
	}

	if ((d = ba_device_lookup(a, &addr.l2_bdaddr)) == NULL) {
		error("Couldn't lookup device: %s", addrstr);
		goto cleanup;
	}

	char path[256];
	snprintf(path, sizeof(path), "%s/asha", d->bluez_dbus_path);
	if ((t = ba_transport_lookup(d, path)) == NULL) {
		error("Couldn't lookup transport: %s", path);
		goto cleanup;
	}

	ba_transport_stop(t);

	pthread_mutex_lock(&t->bt_fd_mtx);
	t->bt_fd = fd;
	t->mtu_read = mtu_rx;
	t->mtu_write = mtu_tx;
	fd = -1;
	pthread_mutex_unlock(&t->bt_fd_mtx);

	ba_transport_pcm_state_set_idle(&t->media.pcm);
	ba_transport_pcm_state_set_idle(&t->media.pcm_bc);

cleanup:
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
	if (fd != -1)
		close(fd);
	return G_SOURCE_CONTINUE;
}

/**
 * Setup ASHA connection dispatcher for incoming audio links. */
static error_code_t asha_setup_connection_dispatcher(
		BluetoothASHA * asha) {

	int fd;
	if ((fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)) == -1)
		return ERROR_SYSTEM(errno);

	struct sockaddr_l2 addr = {
		.l2_family = AF_BLUETOOTH,
		.l2_bdaddr = *BDADDR_ANY,
		.l2_bdaddr_type = BDADDR_LE_PUBLIC };
	/* Request dynamic PSM allocation for connection dispatcher. */
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
		return close(fd), ERROR_SYSTEM(errno);

	socklen_t len = sizeof(addr);
	/* Retrieve dynamically assigned PSM. */
	if (getsockname(fd, (struct sockaddr *)&addr, &len) == -1)
		return close(fd), ERROR_SYSTEM(errno);

	if (listen(fd, 5) == -1)
		return close(fd), ERROR_SYSTEM(errno);

	g_autoptr(GIOChannel) ch = g_io_channel_unix_raw_new(fd);
	/* Attach connection dispatcher to the default main context. */
	asha->dispatcher = g_io_create_watch_full(ch, G_PRIORITY_DEFAULT,
			G_IO_IN, asha_connection_dispatcher, asha, NULL);
	asha->psm = addr.l2_psm;

	debug("Created ASHA connection dispatcher [PSM=%#x]: %d", addr.l2_psm, fd);
	return ERROR_CODE_OK;
}

/**
 * Bind transport to ASHA service based on the GATT accessing device. */
static error_code_t asha_bind_transport(
		BluetoothASHA * asha,
		GDBusMethodInvocation * inv) {

	GVariant * params = g_dbus_method_invocation_get_parameters(inv);
	g_autoptr(GVariant) params_ = g_variant_get_child_value(params, 0);

	const char * device;
	if (!g_variant_lookup(params_, "device", "&o", &device))
		return ERROR_SYSTEM(EINVAL);

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device, &addr);

	/* For now, only one transport can be associated with ASHA service
	 * at a time. This limitation is imposed by BlueZ AcquireNotify/Write
	 * methods which are called only once per characteristic. */
	if (asha->t != NULL) {
		/* Check if the request comes from the same device. */
		if (bacmp(&asha->t->d->addr, &addr) == 0)
			return ERROR_CODE_OK;
		char addrstr[18];
		ba2str(&asha->t->d->addr, addrstr);
		warn("ASHA transport in use by another device: %s", addrstr);
		return ERROR_SYSTEM(EALREADY);
	}

	struct ba_device * d;
	struct ba_adapter * a = asha->a;
	if ((d = ba_device_lookup(a, &addr)) == NULL &&
			(d = ba_device_new(a, &addr)) == NULL) {
		error("Couldn't create new device: %s", device);
		return ERROR_SYSTEM(errno);
	}

	char path[256];
	snprintf(path, sizeof(path), "%s/asha", d->bluez_dbus_path);

	struct ba_transport * t;
	if ((t = ba_transport_lookup(d, path)) == NULL)
		t = ba_transport_new_asha(d, BA_TRANSPORT_PROFILE_ASHA_SINK, ":0",
				path, config.asha.id);
	ba_device_unref(d);

	if (t == NULL) {
		error("Couldn't create ASHA transport: %s", path);
		return ERROR_SYSTEM(errno);
	}

	/* Assign transport to ASHA service. */
	asha->t = t;

	return ERROR_CODE_OK;
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

/**
 * Update ASHA audio status point value and send notification. */
static void asha_status_write(BluetoothASHA * asha, int8_t status) {

	/* Update local status value. */
	asha->status = status;

	/* Notify only if notification channel is available. */
	if (asha->chr_status_notify_channel == NULL)
		return;

	size_t tmp;
	g_autoptr(GError) err = NULL;
	if (g_io_channel_write_chars(asha->chr_status_notify_channel, (void *)&status,
				sizeof(status), &tmp, &err) != G_IO_STATUS_NORMAL) {
		error("Couldn't write ASHA status notification: %s", err->message);
	}

}

static bool asha_characteristic_props_read_value(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothASHA * asha = userdata;

	error_code_t rc;
	if ((rc = asha_bind_transport(asha, inv)) != ERROR_CODE_OK) {
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED,
				error_code_strerror(rc));
		return false;
	}

	const int delay_ms = ba_transport_pcm_delay_get(&asha->t->media.pcm) / 10;
	const asha_properties_t props = {
		.version = ASHA_VERSION_1_0,
		.caps = { .side = config.asha.side, .binaural = config.asha.binaural },
		.id = config.asha.id,
		.features = ASHA_FEATURE_LE_COC_AUDIO,
		.delay = htole16(delay_ms),
		/* The bitmask with supported codecs. */
		.codecs = HTOLE16(1 << ASHA_CODEC_G722),
	};

	GVariant * rv = g_variant_new_fixed_byte_array(&props, sizeof(props));
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(&rv, 1));

	return true;
}

static int asha_ctrl_read(
		GIOChannel * ch,
		G_GNUC_UNUSED GIOCondition cond,
		void * userdata) {
	BluetoothASHA * asha = userdata;

	struct ba_transport * t = asha->t;

	struct {
		uint8_t opcode;
		union {
			asha_ctrl_start_t start;
			asha_ctrl_status_t status;
		};
	} __attribute__((packed)) packet = { 0 };
	size_t len;

	g_autoptr(GError) err = NULL;
	switch (g_io_channel_read_chars(ch, (char *)&packet, sizeof(packet), &len, &err)) {
	case G_IO_STATUS_AGAIN:
		return G_SOURCE_CONTINUE;
	case G_IO_STATUS_ERROR:
		error("ASHA control point read error: %s", err->message);
		return G_SOURCE_CONTINUE;
	case G_IO_STATUS_NORMAL:
		if (t == NULL)
			warn("Received ASHA control command prior to connection establishment");
		switch (packet.opcode) {
		case ASHA_CTRL_OP_START:
			debug("ASHA control point opcode: START");
			/* Verify that the selected codec is supported. */
			if (packet.start.codec != ASHA_CODEC_G722) {
				error("Unsupported ASHA codec: %#x", packet.start.codec);
				asha_status_write(asha, ASHA_STATUS_OP_INVALID_PARAM);
				break;
			}
			if (t != NULL)
				ba_transport_start(t);
			asha_status_write(asha, ASHA_STATUS_OP_OK);
			break;
		case ASHA_CTRL_OP_STOP:
			debug("ASHA control point opcode: STOP");
			if (t != NULL)
				ba_transport_stop(t);
			asha_status_write(asha, ASHA_STATUS_OP_OK);
			break;
		case ASHA_CTRL_OP_STATUS:
			debug("ASHA control point opcode: STATUS");
			/* This opcode does not expect any status notification. */
			break;
		default:
			warn("Unknown ASHA control point opcode: %#x", packet.opcode);
			asha_status_write(asha, ASHA_STATUS_OP_UNKNOWN_COMMAND);
			break;
		}
		return G_SOURCE_CONTINUE;
	case G_IO_STATUS_EOF:
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

static bool asha_characteristic_ctrl_acquire_write(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothASHA * asha = userdata;

	error_code_t rc;
	if ((rc = asha_bind_transport(asha, inv)) != ERROR_CODE_OK) {
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED,
				error_code_strerror(rc));
		return false;
	}

	int fds[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == -1) {
		error("Couldn't create ASHA control point socket pair: %s", strerror(errno));
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED, strerror(errno));
		return false;
	}

	g_autoptr(GIOChannel) ch = g_io_channel_unix_raw_new(fds[0]);
	asha->chr_ctrl_write_watch = g_io_create_watch_full(ch, G_PRIORITY_DEFAULT,
			G_IO_IN, asha_ctrl_read, asha, NULL);

	g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, chr_get_mtu(inv)), fd_list);
	return true;
}

static bool asha_characteristic_status_read_value(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, void * userdata) {
	const BluetoothASHA * asha = userdata;
	GVariant * rv = g_variant_new_fixed_byte_array(&asha->status, sizeof(asha->status));
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(&rv, 1));
	return true;
}

static int asha_status_hup(
		G_GNUC_UNUSED GIOChannel * ch,
		G_GNUC_UNUSED GIOCondition cond,
		void * userdata) {
	BluetoothASHA * asha = userdata;
	debug("Releasing ASHA status notify link: HUP received");
	/* Remove transport association. */
	ba_transport_destroy(g_steal_pointer(&asha->t));
	/* Free notification channel and its watch. */
	g_io_channel_unref(g_steal_pointer(&asha->chr_status_notify_channel));
	g_source_unref(g_steal_pointer(&asha->chr_status_notify_watch_hup));
	/* Remove channel from watch. */
	return G_SOURCE_REMOVE;
}

static bool asha_characteristic_status_acquire_notify(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	BluetoothASHA * asha = userdata;

	error_code_t rc;
	if ((rc = asha_bind_transport(asha, inv)) != ERROR_CODE_OK) {
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED,
				error_code_strerror(rc));
		return false;
	}

	int fds[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == -1) {
		error("Couldn't create ASHA status notify socket pair: %s", strerror(errno));
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED, strerror(errno));
		return false;
	}

	asha->chr_status_notify_channel = g_io_channel_unix_raw_new(fds[0]);
	/* Setup IO watch for checking HUP condition on the socket. HUP means
	 * that the client does not want to receive notifications anymore. */
	asha->chr_status_notify_watch_hup = g_io_create_watch_full(asha->chr_status_notify_channel,
			G_PRIORITY_DEFAULT, G_IO_HUP, asha_status_hup, asha, NULL);

	g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, chr_get_mtu(inv)), fd_list);
	return true;
}

static bool asha_characteristic_volume_read_value(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothASHA * asha = userdata;

	error_code_t rc;
	if ((rc = asha_bind_transport(asha, inv)) != ERROR_CODE_OK) {
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED,
				error_code_strerror(rc));
		return false;
	}

	int volume = ba_transport_pcm_volume_level_to_range(
			asha->t->media.pcm.volume[0].level, 128);
	const int8_t value = volume - 128;
	GVariant * rv = g_variant_new_fixed_byte_array(&value, sizeof(value));
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(&rv, 1));
	return true;
}

static int asha_volume_read(
		GIOChannel * ch,
		G_GNUC_UNUSED GIOCondition cond,
		void * userdata) {
	BluetoothASHA * asha = userdata;

	struct ba_transport * t = asha->t;
	/* Received volume is in range [-128, 0], where -128 means muted. */
	int8_t volume;
	size_t len;

	g_autoptr(GError) err = NULL;
	switch (g_io_channel_read_chars(ch, (char *)&volume, sizeof(volume), &len, &err)) {
	case G_IO_STATUS_AGAIN:
		return G_SOURCE_CONTINUE;
	case G_IO_STATUS_ERROR:
		error("ASHA volume read error: %s", err->message);
		return G_SOURCE_CONTINUE;
	case G_IO_STATUS_NORMAL:
		if (t == NULL)
			warn("Received ASHA volume read prior to connection establishment");
		else {
			bool muted = volume == -128;
			int level = ba_transport_pcm_volume_range_to_level((int)volume + 128, 128);
			debug("Updating ASHA volume: %d [%.2f dB]", volume, 0.01 * level);
			ba_transport_pcm_volume_set(&t->media.pcm.volume[0], &level, NULL, &muted);
		}
		return G_SOURCE_CONTINUE;
	case G_IO_STATUS_EOF:
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

static bool asha_characteristic_volume_acquire_write(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	BluetoothASHA * asha = userdata;

	error_code_t rc;
	if ((rc = asha_bind_transport(asha, inv)) != ERROR_CODE_OK) {
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED,
				error_code_strerror(rc));
		return false;
	}

	int fds[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == -1) {
		error("Couldn't create ASHA volume socket pair: %s", strerror(errno));
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED, strerror(errno));
		return false;
	}

	g_autoptr(GIOChannel) ch = g_io_channel_unix_raw_new(fds[0]);
	asha->chr_volume_write_watch = g_io_create_watch_full(ch, G_PRIORITY_DEFAULT,
			G_IO_IN, asha_volume_read, asha, NULL);

	g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, chr_get_mtu(inv)), fd_list);
	return true;
}

static bool asha_characteristic_psm_read_value(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, void * userdata) {
	BluetoothASHA * asha = userdata;

	error_code_t rc;
	if ((rc = asha_bind_transport(asha, inv)) != ERROR_CODE_OK) {
		g_dbus_method_invocation_return_dbus_error(inv, BLUEZ_ERROR_FAILED,
				error_code_strerror(rc));
		return false;
	}

	/* Send PSM in little-endian format. */
	uint16_t value = htole16(asha->psm);
	GVariant * rv = g_variant_new_fixed_byte_array(&value, sizeof(value));
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(&rv, 1));
	return true;
}

static void app_register_finish(
		GObject * source, GAsyncResult * result, void * userdata) {
	BluetoothASHA * asha = userdata;

	g_autoptr(GError) err = NULL;
	BluetoothGATTApplication * app = BLUETOOTH_GATT_APPLICATION(source);
	if (!bluetooth_gatt_application_register_finish(app, result, &err))
		error("Couldn't register ASHA GATT application: %s", err->message);
	else if (config.asha.advertise)
		bluetooth_advertising_register(asha->adv, asha->a, NULL, NULL);

}

/**
 * Create new Bluetooth ASHA GATT application.
 *
 * @param adapter Adapter on which the ASHA application will be registered.
 * @param path The root D-Bus object path of the ASHA application.
 * @return The ASHA GATT application. The returned object should be freed
 *   with g_object_unref(). */
BluetoothASHA * bluetooth_asha_new(
		struct ba_adapter * adapter,
		const char * path) {

	BluetoothASHA * asha = g_object_new(BLUETOOTH_TYPE_ASHA, NULL);
	asha->a = ba_adapter_ref(adapter);
	asha->path = g_strdup(path);

	/* Setup GATT application for ASHA server. */
	asha->app = bluetooth_gatt_application_new(path);

	g_autoptr(BluetoothGATTService) srv;
	srv = bluetooth_gatt_service_new("/service", BT_UUID_ASHA, true);
	bluetooth_gatt_application_add_service(asha->app, srv);

	g_autoptr(BluetoothGATTCharacteristic) chr_props;
	chr_props = bluetooth_gatt_characteristic_new("/props", BT_UUID_ASHA_PROPS);
	bluetooth_gatt_application_add_service_characteristic(asha->app, srv, chr_props);
	bluetooth_gatt_characteristic_set_flags(chr_props,
			(const char * const[]){"read", NULL});
	bluetooth_gatt_characteristic_set_read_callback(chr_props,
			asha_characteristic_props_read_value, asha);

	g_autoptr(BluetoothGATTCharacteristic) chr_ctrl;
	chr_ctrl = bluetooth_gatt_characteristic_new("/ctrl", BT_UUID_ASHA_CTRL);
	bluetooth_gatt_application_add_service_characteristic(asha->app, srv, chr_ctrl);
	bluetooth_gatt_characteristic_set_flags(chr_ctrl,
			(const char * const[]){"write", "write-without-response", NULL});
	bluetooth_gatt_characteristic_set_acquire_write_callback(chr_ctrl,
			asha_characteristic_ctrl_acquire_write, asha);

	g_autoptr(BluetoothGATTCharacteristic) chr_status;
	chr_status = bluetooth_gatt_characteristic_new("/status", BT_UUID_ASHA_STATUS);
	bluetooth_gatt_application_add_service_characteristic(asha->app, srv, chr_status);
	bluetooth_gatt_characteristic_set_flags(chr_status,
			(const char * const[]){"read", "notify", NULL});
	bluetooth_gatt_characteristic_set_read_callback(chr_status,
			asha_characteristic_status_read_value, asha);
	bluetooth_gatt_characteristic_set_acquire_notify_callback(chr_status,
			asha_characteristic_status_acquire_notify, asha);

	g_autoptr(BluetoothGATTCharacteristic) chr_volume;
	chr_volume = bluetooth_gatt_characteristic_new("/volume", BT_UUID_ASHA_VOLUME);
	bluetooth_gatt_application_add_service_characteristic(asha->app, srv, chr_volume);
	bluetooth_gatt_characteristic_set_flags(chr_volume,
			(const char * const[]){"read", "write", "write-without-response", NULL});
	bluetooth_gatt_characteristic_set_read_callback(chr_volume,
			asha_characteristic_volume_read_value, asha);
	bluetooth_gatt_characteristic_set_acquire_write_callback(chr_volume,
			asha_characteristic_volume_acquire_write, asha);

	g_autoptr(BluetoothGATTCharacteristic) chr_psm;
	chr_psm = bluetooth_gatt_characteristic_new("/psm", BT_UUID_ASHA_PSM);
	bluetooth_gatt_application_add_service_characteristic(asha->app, srv, chr_psm);
	bluetooth_gatt_characteristic_set_flags(chr_psm,
			(const char * const[]){"read", NULL});
	bluetooth_gatt_characteristic_set_read_callback(chr_psm,
			asha_characteristic_psm_read_value, asha);

	/* Setup connection dispatcher for incoming ASHA audio links. */
	if (asha_setup_connection_dispatcher(asha) != ERROR_CODE_OK) {
		g_object_unref(asha);
		return NULL;
	}

	if (config.asha.advertise) {
		char adv_path[256];
		snprintf(adv_path, sizeof(adv_path), "%s/adv", path);
		asha->adv = bluetooth_advertising_new(
				bluetooth_gatt_application_get_object_manager_server(asha->app),
				adv_path, BT_UUID_ASHA, config.asha.name);
		asha_service_data_payload_t payload = {
			.version = ASHA_VERSION_1_0,
			.caps = { .side = config.asha.side, .binaural = config.asha.binaural } };
		/* ASHA LE advertisement does not send whole ID but only the most
		 * significant 4 bytes, hence we need to copy only that part. */
		memcpy(payload.id, &config.asha.id, sizeof(payload.id));
		bluetooth_advertising_set_service_data(asha->adv,
				&payload, sizeof(payload));
	}

	bluetooth_gatt_application_set_connection(asha->app, config.dbus);
	bluetooth_gatt_application_register(asha->app, adapter, app_register_finish, asha);

	return asha;
}
