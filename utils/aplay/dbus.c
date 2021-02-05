/*
 * BlueALSA - dbus.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "dbus.h"

#include <stdbool.h>
#include <string.h>

static bool dbus_message_iter_get_basic_boolean(DBusMessageIter *iter) {
	dbus_bool_t tmp = FALSE;
	return dbus_message_iter_get_basic(iter, &tmp), tmp;
}

static unsigned int dbus_message_iter_get_basic_integer(DBusMessageIter *iter) {
	dbus_uint32_t tmp = 0;
	return dbus_message_iter_get_basic(iter, &tmp), tmp;
}

static const char *dbus_message_iter_get_basic_string(DBusMessageIter *iter) {
	const char *tmp = "";
	return dbus_message_iter_get_basic(iter, &tmp), tmp;
}

DBusMessage *dbus_get_properties(DBusConnection *conn,
		const char *service, const char *path, const char *interface,
		const char *property, DBusError *error) {

	DBusMessage *msg;
	const char *method = property == NULL ? "GetAll" : "Get";
	if ((msg = dbus_message_new_method_call(service, path,
					DBUS_INTERFACE_PROPERTIES, method)) == NULL)
		return NULL;

	DBusMessage *rep = NULL;

	DBusMessageIter iter;
	dbus_message_iter_init_append(msg, &iter);

	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface))
		goto fail;
	if (property != NULL &&
			!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &property))
		goto fail;

	rep = dbus_connection_send_with_reply_and_block(conn, msg,
			DBUS_TIMEOUT_USE_DEFAULT, error);

fail:
	dbus_message_unref(msg);
	return rep;
}

int dbus_bluez_get_device(DBusConnection *conn, const char *path,
		struct bluez_device *dev, DBusError *error) {

	char path_addr[sizeof("00:00:00:00:00:00")] = { 0 };
	char *tmp;
	size_t i;

	memset(dev, 0, sizeof(*dev));
	strncpy(dev->path, path, sizeof(dev->path) - 1);

	/* Try to extract BT MAC address from the D-Bus path. We will use it as
	 * a fallback in case where BlueZ service is not available on the bus -
	 * usage with bluealsa-mock server. */
	if ((tmp = strstr(path, "/dev_")) != NULL)
		strncpy(path_addr, tmp + 5, sizeof(path_addr) - 1);
	for (i = 0; i < sizeof(path_addr); i++)
		if (path_addr[i] == '_')
			path_addr[i] = ':';
	str2ba(path_addr, &dev->bt_addr);

	DBusMessage *rep;
	if ((rep = dbus_get_properties(conn, "org.bluez", path,
					"org.bluez.Device1", NULL, error)) == NULL)
		return -1;

	DBusMessageIter iter;
	dbus_message_iter_init(rep, &iter);

	DBusMessageIter iter_dict;
	for (dbus_message_iter_recurse(&iter, &iter_dict);
			dbus_message_iter_get_arg_type(&iter_dict) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_dict)) {

		DBusMessageIter iter_entry;
		DBusMessageIter iter_entry_val;
		const char *key;

		dbus_message_iter_recurse(&iter_dict, &iter_entry);
		dbus_message_iter_get_basic(&iter_entry, &key);
		dbus_message_iter_next(&iter_entry);
		dbus_message_iter_recurse(&iter_entry, &iter_entry_val);

		if (strcmp(key, "Adapter") == 0) {
			const char *tmp;
			if ((tmp = strrchr(dbus_message_iter_get_basic_string(&iter_entry_val), '/')) != NULL)
				strncpy(dev->hci_name, tmp + 1, sizeof(dev->hci_name) - 1);
		}
		else if (strcmp(key, "Address") == 0)
			str2ba(dbus_message_iter_get_basic_string(&iter_entry_val), &dev->bt_addr);
		else if (strcmp(key, "Alias") == 0)
			strncpy(dev->name, dbus_message_iter_get_basic_string(&iter_entry_val), sizeof(dev->name) - 1);
		else if (strcmp(key, "Class") == 0)
			dev->class_ = dbus_message_iter_get_basic_integer(&iter_entry_val);
		else if (strcmp(key, "Icon") == 0)
			strncpy(dev->icon, dbus_message_iter_get_basic_string(&iter_entry_val), sizeof(dev->icon) - 1);
		else if (strcmp(key, "Blocked") == 0)
			dev->blocked = dbus_message_iter_get_basic_boolean(&iter_entry_val);
		else if (strcmp(key, "Connected") == 0)
			dev->connected = dbus_message_iter_get_basic_boolean(&iter_entry_val);
		else if (strcmp(key, "Paired") == 0)
			dev->paired = dbus_message_iter_get_basic_boolean(&iter_entry_val);
		else if (strcmp(key, "Trusted") == 0)
			dev->trusted = dbus_message_iter_get_basic_boolean(&iter_entry_val);

	}

	dbus_message_unref(rep);
	return 0;
}
