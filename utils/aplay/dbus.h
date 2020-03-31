/*
 * BlueALSA - dbus.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_APLAY_DBUS_H_
#define BLUEALSA_APLAY_DBUS_H_

#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

struct bluez_device {

	/* BlueZ D-Bus device path */
	char path[128];

	/* used HCI adapter */
	char hci_name[8];
	/* device MAC address */
	bdaddr_t bt_addr;

	/* device name (alias) */
	char name[64];

	/* device class */
	unsigned int class_;
	char icon[32];

	unsigned int blocked : 1;
	unsigned int connected : 1;
	unsigned int paired : 1;
	unsigned int trusted : 1;

};

DBusMessage *dbus_get_properties(DBusConnection *conn,
		const char *service, const char *path, const char *interface,
		const char *property, DBusError *error);

int dbus_bluez_get_device(DBusConnection *conn, const char *path,
		struct bluez_device *dev, DBusError *error);

#endif
