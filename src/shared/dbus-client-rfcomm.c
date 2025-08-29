/*
 * BlueALSA - dbus-rfcomm.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "dbus-client-rfcomm.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "defs.h"

/**
 * Callback function for rfcomm object properties parser. */
static dbus_bool_t ba_dbus_message_iter_rfcomm_props_get_cb(const char *key,
		DBusMessageIter *value, void *userdata, DBusError *error) {
	struct ba_rfcomm_props *props = (struct ba_rfcomm_props *)userdata;

	char type;
	if ((type = dbus_message_iter_get_arg_type(value)) != DBUS_TYPE_VARIANT) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
				"Incorrect property value type: %c != %c", type, DBUS_TYPE_VARIANT);
		return FALSE;
	}

	DBusMessageIter variant;
	dbus_message_iter_recurse(value, &variant);
	type = dbus_message_iter_get_arg_type(&variant);

	char type_expected;

	if (strcmp(key, "Transport") == 0) {

		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;

		const char *tmp;
		dbus_message_iter_get_basic(&variant, &tmp);
		strncpy(props->transport, tmp, sizeof(props->transport) - 1);

	}
	else if (strcmp(key, "Features") == 0) {

		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;

		const char *tmp[32];
		size_t length = ARRAYSIZE(tmp);
		if (!dbus_message_iter_array_get_strings(&variant, error, tmp, &length))
			return FALSE;

		props->features = malloc(length * sizeof(*props->features));
		props->features_len = MIN(length, ARRAYSIZE(tmp));
		for (size_t i = 0; i < length; i++)
			props->features[i] = strdup(tmp[i]);

	}
	else if (strcmp(key, "Battery") == 0) {

		if (type != (type_expected = DBUS_TYPE_BYTE))
			goto fail;

		signed char level;
		dbus_message_iter_get_basic(&variant, &level);
		props->battery = level;

	}

	return TRUE;

fail:
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;
}

/**
 * Get properties of BlueALSA RFCOMM object.
 *
 * This function allocates resources within the properties structure, which
 * shall be freed with the ba_dbus_rfcomm_props_free() function. */
dbus_bool_t ba_dbus_rfcomm_props_get(
		struct ba_dbus_ctx *ctx,
		const char *rfcomm_path,
		struct ba_rfcomm_props *props,
		DBusError *error) {

	props->features = NULL;
	props->features_len = 0;

	return ba_dbus_props_get_all(ctx,
			rfcomm_path, BLUEALSA_INTERFACE_RFCOMM, error,
			ba_dbus_message_iter_rfcomm_props_get_cb, props);
}

/**
 * Free BlueALSA RFCOMM properties structure. */
void ba_dbus_rfcomm_props_free(
		struct ba_rfcomm_props *props) {
	if (props->features != NULL) {
		for (size_t i = 0; i < props->features_len; i++)
			free(props->features[i]);
		free(props->features);
		props->features = NULL;
	}
}

/**
 * Open BlueALSA RFCOMM socket for dispatching AT commands. */
dbus_bool_t ba_dbus_rfcomm_open(
		struct ba_dbus_ctx *ctx,
		const char *rfcomm_path,
		int *fd_rfcomm,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, rfcomm_path,
					BLUEALSA_INTERFACE_RFCOMM, "Open")) == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL) {
		dbus_message_unref(msg);
		return FALSE;
	}

	dbus_bool_t rv;
	rv = dbus_message_get_args(rep, error,
			DBUS_TYPE_UNIX_FD, fd_rfcomm,
			DBUS_TYPE_INVALID);

	dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}
