/*
 * BlueALSA - media-control.c
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include <dbus/dbus.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "media-control.h"
#include "shared/dbus-client.h"
#include "shared/log.h"

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

extern void io_worker_update_player_path(
		const char * device_path,
		struct media_control * mc);

extern void io_worker_update_player_status(
		const char * player_path,
		const char * status,
		DBusConnection * conn);

static dbus_bool_t media_control_dbus_message_iter_get_props_cb(
		const char * key,
		DBusMessageIter * val,
		void * data,
		DBusError * err) {

	struct media_control * mc = data;
	char type;

	if ((type = dbus_message_iter_get_arg_type(val)) != DBUS_TYPE_VARIANT) {
		dbus_set_error(err, DBUS_ERROR_INVALID_SIGNATURE,
		               "Incorrect property value type: %c != %c", type, DBUS_TYPE_VARIANT);
		return FALSE;
	}

	DBusMessageIter variant;
	dbus_message_iter_recurse(val, &variant);
	type = dbus_message_iter_get_arg_type(&variant);

	char type_expected;

	if (strcmp(key, "Player") == 0) {
		if (type != (type_expected = DBUS_TYPE_OBJECT_PATH))
			goto fail;

		const char * tmp;
		dbus_message_iter_get_basic(&variant, &tmp);
		strncpy(mc->player_path, tmp, sizeof(mc->player_path) - 1);
		/* The player path is only ever set when the control API is connected */
		mc->connected = true;
	}
	else if (strcmp(key, "Connected") == 0) {
		if (type != (type_expected = DBUS_TYPE_BOOLEAN))
			goto fail;

		dbus_bool_t connected;
		dbus_message_iter_get_basic(&variant, &connected);
		mc->connected = connected;
		if (!connected)
			mc->player_path[0] = 0;
	}

	return TRUE;

fail:
	dbus_set_error(err, DBUS_ERROR_INVALID_SIGNATURE,
	               "Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;

}

/**
 * Parse Bluez MediaControl properties. */
static dbus_bool_t dbus_message_iter_get_media_control_props(
		DBusMessageIter * iter,
		struct media_control * mc) {
	return dbus_message_iter_dict(iter, NULL,
	                              media_control_dbus_message_iter_get_props_cb, mc);
}

/**
 * Parse Bluez MediaPlayer properties. */
static dbus_bool_t dbus_message_iter_get_media_player_status(
		DBusMessageIter * iter,
		const char ** status) {

	DBusMessageIter dict_iter, entry_iter, variant_iter;
	dbus_message_iter_recurse(iter, &dict_iter);

	const char * val = NULL;
	while (dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_DICT_ENTRY) {
		const char * key;
		dbus_message_iter_recurse(&dict_iter, &entry_iter);
		dbus_message_iter_get_basic(&entry_iter, &key);
		dbus_message_iter_next(&entry_iter);
		dbus_message_iter_recurse(&entry_iter, &variant_iter);
		if (strcmp(key, "Status") == 0 &&
		    dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&variant_iter, &val);
			break;
		}
		dbus_message_iter_next(&dict_iter);
	}
	if (val != NULL)
		*status = val;

	return val != NULL;
}

static DBusHandlerResult media_control_dbus_signal_handler(
		DBusConnection * conn,
		DBusMessage * message,
		void * data) {
	(void)data;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(dbus_message_get_interface(message), DBUS_IFACE_PROPERTIES) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char * path = dbus_message_get_path(message);
	DBusMessageIter iter;

	if (!dbus_message_iter_init(message, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
		error("Invalid DBus PropertiesChanged signal signature");
		goto fail;
	}

	const char * prop_interface;
	dbus_message_iter_get_basic(&iter, &prop_interface);

	if (strcmp(prop_interface, BLUEZ_IFACE_MEDIACONTROL) == 0) {
		dbus_message_iter_next(&iter);
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
			error("Invalid DBus PropertiesChanged signal content");
			goto fail;
		}

		struct media_control mc = { 0 };
		if (!dbus_message_iter_get_media_control_props(&iter, &mc))
			goto fail;
		io_worker_update_player_path(path, &mc);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(prop_interface, BLUEZ_IFACE_MEDIAPLAYER) == 0) {
		dbus_message_iter_next(&iter);
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
			error("Invalid DBus PropertiesChanged signal content");
			goto fail;
		}

		const char * status = NULL;
		if (!dbus_message_iter_get_media_player_status(&iter, &status))
			goto fail;

		io_worker_update_player_status(path, status, conn);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

}

bool media_control_init(struct media_control * mc, const char * device_path,
		DBusConnection * conn) {

	struct ba_dbus_ctx bluez_ctx = {
		.conn = conn,
		.ba_service = BLUEZ_SERVICE,
	};

	DBusError error = DBUS_ERROR_INIT;
	dbus_bool_t rv;

	rv = ba_dbus_props_get_all(
		&bluez_ctx,
		device_path,
		BLUEZ_IFACE_MEDIACONTROL,
		&error,
		media_control_dbus_message_iter_get_props_cb,
		mc);

	if (!rv) {
		debug("Failed to get media player: %s", error.message);
		dbus_error_free(&error);
	}

	mc->state = MEDIA_CONTROL_STATE_UNKNOWN;

	return rv;
}

dbus_bool_t media_control_dbus_add_signal_handler(struct ba_dbus_ctx * dbus_ctx) {
	ba_dbus_connection_signal_match_add(
		dbus_ctx,
		BLUEZ_SERVICE,
		NULL,
		DBUS_INTERFACE_PROPERTIES,
		"PropertiesChanged",
		"arg0='" BLUEZ_IFACE_MEDIACONTROL "'");

	ba_dbus_connection_signal_match_add(
		dbus_ctx,
		BLUEZ_SERVICE,
		NULL,
		DBUS_INTERFACE_PROPERTIES,
		"PropertiesChanged",
		"arg0='" BLUEZ_IFACE_MEDIAPLAYER "'");

	if (!dbus_connection_add_filter(dbus_ctx->conn, media_control_dbus_signal_handler, NULL, NULL)) {
		error("Couldn't add media_control D-Bus filter");
		return FALSE;
	}

	return TRUE;
}

bool media_control_pause_player(const struct media_control * mc, DBusConnection * conn) {

	if (!mc->connected)
		return false;

	DBusMessage * msg = NULL, * rep = NULL;
	DBusError err = DBUS_ERROR_INIT;
	bool rv = true;

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, mc->player_path, BLUEZ_IFACE_MEDIAPLAYER,
	                                   "Pause");

	if (!dbus_connection_send(conn, msg, NULL)) {
		warn("Couldn't pause player: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	goto final;

fail:
	rv = false;

final:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return rv;
}
