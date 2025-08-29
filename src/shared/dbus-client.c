/*
 * BlueALSA - dbus-client.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "dbus-client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "defs.h"

static dbus_bool_t ba_dbus_watch_add(DBusWatch *watch, void *data) {
	struct ba_dbus_ctx *ctx = (struct ba_dbus_ctx *)data;
	DBusWatch **tmp = ctx->watches;
	if ((tmp = realloc(tmp, (ctx->watches_len + 1) * sizeof(*tmp))) == NULL)
		return FALSE;
	tmp[ctx->watches_len++] = watch;
	ctx->watches = tmp;
	return TRUE;
}

static void ba_dbus_watch_del(DBusWatch *watch, void *data) {
	struct ba_dbus_ctx *ctx = (struct ba_dbus_ctx *)data;
	for (size_t i = 0; i < ctx->watches_len; i++)
		if (ctx->watches[i] == watch)
			ctx->watches[i] = ctx->watches[--ctx->watches_len];
}

static void ba_dbus_watch_toggled(DBusWatch *watch, void *data) {
	(void)watch;
	(void)data;
}

dbus_bool_t ba_dbus_connection_ctx_init(
		struct ba_dbus_ctx *ctx,
		const char *ba_service_name,
		DBusError *error) {

	/* Zero-out context structure, so it will be
	 * safe to call *_ctx_free() upon error. */
	memset(ctx, 0, sizeof(*ctx));

	if ((ctx->conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, error)) == NULL)
		return FALSE;

	/* do not terminate in case of D-Bus connection being lost */
	dbus_connection_set_exit_on_disconnect(ctx->conn, FALSE);

	if (!dbus_connection_set_watch_functions(ctx->conn, ba_dbus_watch_add,
				ba_dbus_watch_del, ba_dbus_watch_toggled, ctx, NULL)) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	strncpy(ctx->ba_service, ba_service_name, sizeof(ctx->ba_service) - 1);

	return TRUE;
}

void ba_dbus_connection_ctx_free(
		struct ba_dbus_ctx *ctx) {
	if (ctx->conn != NULL) {
		dbus_connection_close(ctx->conn);
		dbus_connection_unref(ctx->conn);
		ctx->conn = NULL;
	}
	if (ctx->watches != NULL) {
		free(ctx->watches);
		ctx->watches = NULL;
	}
	if (ctx->matches != NULL) {
		for (size_t i = 0; i < ctx->matches_len; i++)
			free(ctx->matches[i]);
		free(ctx->matches);
		ctx->matches = NULL;
	}
}

dbus_bool_t ba_dbus_connection_signal_match_add(
		struct ba_dbus_ctx *ctx,
		const char *sender,
		const char *path,
		const char *iface,
		const char *member,
		const char *extra) {

	char match[512] = "type='signal'";
	size_t len = 13;

	if (sender != NULL) {
		snprintf(&match[len], sizeof(match) - len, ",sender='%s'", sender);
		len += strlen(&match[len]);
	}
	if (path != NULL) {
		snprintf(&match[len], sizeof(match) - len, ",path='%s'", path);
		len += strlen(&match[len]);
	}
	if (iface != NULL) {
		snprintf(&match[len], sizeof(match) - len, ",interface='%s'", iface);
		len += strlen(&match[len]);
	}
	if (member != NULL) {
		snprintf(&match[len], sizeof(match) - len, ",member='%s'", member);
		len += strlen(&match[len]);
	}
	if (extra != NULL)
		snprintf(&match[len], sizeof(match) - len, ",%s", extra);

	char **tmp = ctx->matches;
	size_t tmp_len = ctx->matches_len;
	if ((tmp = realloc(tmp, (tmp_len + 1) * sizeof(*tmp))) == NULL)
		return FALSE;
	ctx->matches = tmp;
	if ((ctx->matches[tmp_len] = strdup(match)) == NULL)
		return FALSE;
	ctx->matches_len++;

	dbus_bus_add_match(ctx->conn, match, NULL);
	return TRUE;
}

dbus_bool_t ba_dbus_connection_signal_match_clean(
		struct ba_dbus_ctx *ctx) {

	for (size_t i = 0; i < ctx->matches_len; i++) {
		dbus_bus_remove_match(ctx->conn, ctx->matches[i], NULL);
		free(ctx->matches[i]);
	}

	ctx->matches_len = 0;
	return TRUE;
}

/**
 * Dispatch D-Bus messages synchronously. */
dbus_bool_t ba_dbus_connection_dispatch(
		struct ba_dbus_ctx *ctx) {

	struct pollfd fds[8];
	nfds_t nfds = ARRAYSIZE(fds);

	ba_dbus_connection_poll_fds(ctx, fds, &nfds);
	if (poll(fds, nfds, 0) > 0)
		ba_dbus_connection_poll_dispatch(ctx, fds, nfds);

	/* Dispatch incoming D-Bus messages/signals. The actual dispatching is
	 * done in a function registered with dbus_connection_add_filter(). */
	while (dbus_connection_dispatch(ctx->conn) == DBUS_DISPATCH_DATA_REMAINS)
		continue;

	return TRUE;
}

dbus_bool_t ba_dbus_connection_poll_fds(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t *nfds) {

	if (*nfds < ctx->watches_len) {
		*nfds = ctx->watches_len;
		return FALSE;
	}

	for (size_t i = 0; i < ctx->watches_len; i++) {
		DBusWatch *watch = ctx->watches[i];

		fds[i].fd = -1;
		fds[i].events = 0;

		if (dbus_watch_get_enabled(watch))
			fds[i].fd = dbus_watch_get_unix_fd(watch);
		if (dbus_watch_get_flags(watch) & DBUS_WATCH_READABLE)
			fds[i].events = POLLIN;

	}

	*nfds = ctx->watches_len;
	return TRUE;
}

dbus_bool_t ba_dbus_connection_poll_dispatch(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t nfds) {

	dbus_bool_t rv = FALSE;

	if (nfds > ctx->watches_len)
		nfds = ctx->watches_len;

	for (size_t i = 0; i < nfds; i++)
		if (fds[i].revents) {
			unsigned int flags = 0;
			if (fds[i].revents & POLLIN)
				flags |= DBUS_WATCH_READABLE;
			if (fds[i].revents & POLLOUT)
				flags |= DBUS_WATCH_WRITABLE;
			if (fds[i].revents & POLLERR)
				flags |= DBUS_WATCH_ERROR;
			if (fds[i].revents & POLLHUP)
				flags |= DBUS_WATCH_HANGUP;
			dbus_watch_handle(ctx->watches[i], flags);
			rv = TRUE;
		}

	return rv;
}

dbus_bool_t ba_dbus_props_get_all(
		struct ba_dbus_ctx *ctx,
		const char *path,
		const char *interface,
		DBusError *error,
		dbus_bool_t (*cb)(const char *key, DBusMessageIter *val, void *data, DBusError *err),
		void *userdata) {

	DBusMessage *msg = NULL, *rep = NULL;
	dbus_bool_t rv = FALSE;

	if ((msg = dbus_message_new_method_call(ctx->ba_service, path,
					DBUS_INTERFACE_PROPERTIES, "GetAll")) == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	DBusMessageIter iter;
	dbus_message_iter_init_append(msg, &iter);
	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface)) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	if (!dbus_message_iter_dict(&iter, error, cb, userdata))
		goto fail;

	rv = TRUE;

fail:
	if (rep != NULL)
		dbus_message_unref(rep);
	if (msg != NULL)
		dbus_message_unref(msg);
	return rv;
}

/**
 * Callback function for manager object properties parser. */
static dbus_bool_t bluealsa_dbus_message_iter_get_manager_props_cb(const char *key,
		DBusMessageIter *value, void *userdata, DBusError *error) {
	struct ba_service_props *props = (struct ba_service_props *)userdata;

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

	if (strcmp(key, "Version") == 0) {

		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;

		const char *tmp;
		dbus_message_iter_get_basic(&variant, &tmp);
		strncpy(props->version, tmp, sizeof(props->version) - 1);

	}
	else if (strcmp(key, "Adapters") == 0) {

		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;

		const char *tmp[ARRAYSIZE(props->adapters)];
		size_t length = ARRAYSIZE(tmp);
		if (!dbus_message_iter_array_get_strings(&variant, error, tmp, &length))
			return FALSE;

		props->adapters_len = MIN(length, ARRAYSIZE(tmp));
		for (size_t i = 0; i < length; i++)
			strncpy(props->adapters[i], tmp[i], sizeof(props->adapters[i]) - 1);

	}
	else if (strcmp(key, "Profiles") == 0) {

		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;

		const char *tmp[32];
		size_t length = ARRAYSIZE(tmp);
		if (!dbus_message_iter_array_get_strings(&variant, error, tmp, &length))
			return FALSE;

		props->profiles = malloc(length * sizeof(*props->profiles));
		props->profiles_len = MIN(length, ARRAYSIZE(tmp));
		for (size_t i = 0; i < length; i++)
			props->profiles[i] = strdup(tmp[i]);

	}
	else if (strcmp(key, "Codecs") == 0) {

		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;

		const char *tmp[64];
		size_t length = ARRAYSIZE(tmp);
		if (!dbus_message_iter_array_get_strings(&variant, error, tmp, &length))
			return FALSE;

		props->codecs = malloc(length * sizeof(*props->codecs));
		props->codecs_len = MIN(length, ARRAYSIZE(tmp));
		for (size_t i = 0; i < length; i++)
			props->codecs[i] = strdup(tmp[i]);

	}

	return TRUE;

fail:
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;
}

/**
 * Get properties of BlueALSA service.
 *
 * This function allocates resources within the properties structure, which
 * shall be freed with the ba_dbus_service_props_free() function. */
dbus_bool_t ba_dbus_service_props_get(
		struct ba_dbus_ctx *ctx,
		struct ba_service_props *props,
		DBusError *error) {

	props->profiles = NULL;
	props->profiles_len = 0;
	props->codecs = NULL;
	props->codecs_len = 0;

	return ba_dbus_props_get_all(ctx,
			"/org/bluealsa", BLUEALSA_INTERFACE_MANAGER, error,
			bluealsa_dbus_message_iter_get_manager_props_cb, props);
}

/**
 * Free BlueALSA service properties structure. */
void ba_dbus_service_props_free(
		struct ba_service_props *props) {
	if (props->profiles != NULL) {
		for (size_t i = 0; i < props->profiles_len; i++)
			free(props->profiles[i]);
		free(props->profiles);
		props->profiles = NULL;
	}
	if (props->codecs != NULL) {
		for (size_t i = 0; i < props->codecs_len; i++)
			free(props->codecs[i]);
		free(props->codecs);
		props->codecs = NULL;
	}
}

/**
 * Extract strings from the string array. */
dbus_bool_t dbus_message_iter_array_get_strings(
		DBusMessageIter *iter,
		DBusError *error,
		const char **strings,
		size_t *length) {

	char *signature;
	size_t i;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		goto fail;

	DBusMessageIter iter_array;
	for (dbus_message_iter_recurse(iter, &iter_array), i = 0;
			dbus_message_iter_get_arg_type(&iter_array) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_array)) {
		if (dbus_message_iter_get_arg_type(&iter_array) != DBUS_TYPE_STRING)
			goto fail;
		if (i < *length)
			dbus_message_iter_get_basic(&iter_array, &strings[i]);
		i++;
	}

	*length = i;
	return TRUE;

fail:
	signature = dbus_message_iter_get_signature(iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != as", signature);
	dbus_free(signature);
	return FALSE;
}

/**
 * Call the given function for each key/value pairs. */
dbus_bool_t dbus_message_iter_dict(
		DBusMessageIter *iter,
		DBusError *error,
		dbus_bool_t (*cb)(const char *key, DBusMessageIter *val, void *data, DBusError *err),
		void *userdata) {

	char *signature;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		goto fail;

	DBusMessageIter iter_dict;
	for (dbus_message_iter_recurse(iter, &iter_dict);
			dbus_message_iter_get_arg_type(&iter_dict) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_dict)) {

		DBusMessageIter iter_entry;
		const char *key;

		if (dbus_message_iter_get_arg_type(&iter_dict) != DBUS_TYPE_DICT_ENTRY)
			goto fail;
		dbus_message_iter_recurse(&iter_dict, &iter_entry);
		if (dbus_message_iter_get_arg_type(&iter_entry) != DBUS_TYPE_STRING)
			goto fail;
		dbus_message_iter_get_basic(&iter_entry, &key);
		if (!dbus_message_iter_next(&iter_entry))
			goto fail;

		if (!cb(key, &iter_entry, userdata, error))
			return FALSE;

	}

	return TRUE;

fail:
	signature = dbus_message_iter_get_signature(iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != a{s#}", signature);
	dbus_free(signature);
	return FALSE;
}

/**
 * Append key-value pair with basic type value to the D-Bus message. */
dbus_bool_t dbus_message_iter_dict_append_basic(
		DBusMessageIter *iter,
		const char *key,
		int value_type,
		const void *value) {

	DBusMessageIter entry;
	DBusMessageIter variant;
	const char signature[2] = { value_type, '\0' };

	if (dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry) &&
			dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) &&
			dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant) &&
			dbus_message_iter_append_basic(&variant, value_type, value) &&
			dbus_message_iter_close_container(&entry, &variant) &&
			dbus_message_iter_close_container(iter, &entry))
		return TRUE;

	return FALSE;
}

int dbus_error_to_errno(
		const DBusError *error) {
	if (strcmp(error->name, DBUS_ERROR_NO_MEMORY) == 0)
		return ENOMEM;
	if (strcmp(error->name, DBUS_ERROR_BAD_ADDRESS) == 0)
		return EFAULT;
	if (strcmp(error->name, DBUS_ERROR_SERVICE_UNKNOWN) == 0)
		return ESRCH;
	if (strcmp(error->name, DBUS_ERROR_ACCESS_DENIED) == 0)
		return EACCES;
	if (strcmp(error->name, DBUS_ERROR_NO_REPLY) == 0 ||
			strcmp(error->name, DBUS_ERROR_TIMEOUT) == 0)
		return ETIMEDOUT;
	if (strcmp(error->name, DBUS_ERROR_INVALID_ARGS) == 0)
		return EINVAL;
	if (strcmp(error->name, DBUS_ERROR_FILE_NOT_FOUND) == 0)
		return ENODEV;
	if (strcmp(error->name, DBUS_ERROR_LIMITS_EXCEEDED) == 0)
		return EBUSY;
	return EIO;
}
