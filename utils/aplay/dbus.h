/*
 * BlueALSA - dbus.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifndef BLUEALSA_APLAY_DBUS_H_
#define BLUEALSA_APLAY_DBUS_H_

#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#ifndef DBUS_IFACE_PROPERTIES
# define DBUS_IFACE_PROPERTIES "org.freedesktop.DBus.Properties"
#endif

#ifndef BLUEZ_SERVICE
# define BLUEZ_SERVICE "org.bluez"
#endif

#ifndef BLUEZ_IFACE_MEDIACONTROL
# define BLUEZ_IFACE_MEDIACONTROL BLUEZ_SERVICE ".MediaControl1"
#endif

#ifndef BLUEZ_IFACE_MEDIAPLAYER
# define BLUEZ_IFACE_MEDIAPLAYER BLUEZ_SERVICE ".MediaPlayer1"
#endif

struct bluez_device {

	/* BlueZ D-Bus device path */
	char path[128];

	/* used HCI adapter */
	char hci_name[8];
	/* device MAC address */
	bdaddr_t bt_addr;

	/* device name (alias) */
	char name[64];
	/* device type name */
	char icon[32];

	unsigned int paired : 1;
	unsigned int trusted : 1;

};

DBusMessage * dbus_get_properties(DBusConnection * conn,
		const char * service, const char * path, const char * interface,
		const char * property, DBusError * error);

int dbus_bluez_get_device(DBusConnection * conn, const char * path,
		struct bluez_device * dev, DBusError * error);

#endif
