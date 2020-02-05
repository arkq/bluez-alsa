/*
 * BlueALSA - dbus-client.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/dbus-client.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int path2ba(const char *path, bdaddr_t *ba) {

	unsigned int x[6];
	if ((path = strstr(path, "/dev_")) == NULL ||
			sscanf(&path[5], "%x_%x_%x_%x_%x_%x",
				&x[5], &x[4], &x[3], &x[2], &x[1], &x[0]) != 6)
		return -1;

	size_t i;
	for (i = 0; i < 6; i++)
		ba->b[i] = x[i];

	return 0;
}

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
	size_t i;
	for (i = 0; i < ctx->watches_len; i++)
		if (ctx->watches[i] == watch)
			ctx->watches[i] = ctx->watches[--ctx->watches_len];
}

static void ba_dbus_watch_toggled(DBusWatch *watch, void *data) {
	(void)watch;
	(void)data;
}

dbus_bool_t bluealsa_dbus_connection_ctx_init(
		struct ba_dbus_ctx *ctx,
		const char *ba_service_name,
		DBusError *error) {

	/* Zero-out context structure, so it will be
	 * safe to call *_ctx_free() upon error. */
	memset(ctx, 0, sizeof(*ctx));

	if ((ctx->conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, error)) == NULL)
		return FALSE;

	if (!dbus_connection_set_watch_functions(ctx->conn, ba_dbus_watch_add,
				ba_dbus_watch_del, ba_dbus_watch_toggled, ctx, NULL)) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	strncpy(ctx->ba_service, ba_service_name, sizeof(ctx->ba_service) - 1);

	return TRUE;
}

void bluealsa_dbus_connection_ctx_free(
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
		size_t i;
		for (i = 0; i < ctx->matches_len; i++)
			free(ctx->matches[i]);
		free(ctx->matches);
		ctx->matches = NULL;
	}
}

dbus_bool_t bluealsa_dbus_connection_signal_match_add(
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

dbus_bool_t bluealsa_dbus_connection_signal_match_clean(
		struct ba_dbus_ctx *ctx) {

	size_t i;
	for (i = 0; i < ctx->matches_len; i++) {
		dbus_bus_remove_match(ctx->conn, ctx->matches[i], NULL);
		free(ctx->matches[i]);
	}

	ctx->matches_len = 0;
	return TRUE;
}

dbus_bool_t bluealsa_dbus_connection_poll_fds(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t *nfds) {

	if (*nfds < ctx->watches_len) {
		*nfds = ctx->watches_len;
		return FALSE;
	}

	size_t i;
	for (i = 0; i < ctx->watches_len; i++) {
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

dbus_bool_t bluealsa_dbus_connection_poll_dispatch(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t nfds) {

	dbus_bool_t rv = FALSE;
	size_t i;

	if (nfds > ctx->watches_len)
		nfds = ctx->watches_len;

	for (i = 0; i < nfds; i++)
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

dbus_bool_t bluealsa_dbus_get_pcms(
		struct ba_dbus_ctx *ctx,
		struct ba_pcm **pcms,
		size_t *length,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, "/org/bluealsa",
					BLUEALSA_INTERFACE_MANAGER, "GetPCMs")) == NULL) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	dbus_bool_t rv = TRUE;
	struct ba_pcm *_pcms = NULL;
	char *signature;
	size_t i;

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	DBusMessageIter iter_pcms;
	for (dbus_message_iter_recurse(&iter, &iter_pcms), i = 0;
			dbus_message_iter_get_arg_type(&iter_pcms) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_pcms), i++) {

		if (dbus_message_iter_get_arg_type(&iter_pcms) != DBUS_TYPE_DICT_ENTRY)
			goto fail_signature;

		struct ba_pcm *tmp = _pcms;
		if ((tmp = realloc(tmp, (i + 1) * sizeof(*tmp))) == NULL) {
			dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
			goto fail;
		}

		_pcms = tmp;

		DBusMessageIter iter_pcms_entry;
		dbus_message_iter_recurse(&iter_pcms, &iter_pcms_entry);

		DBusError err = DBUS_ERROR_INIT;
		if (!bluealsa_dbus_message_iter_get_pcm(&iter_pcms_entry, &err, &_pcms[i])) {
			dbus_set_error(error, err.name, "Get PCM: %s", err.message);
			dbus_error_free(&err);
			goto fail;
		}

	}

	*pcms = _pcms;
	*length = i;

	goto success;

fail_signature:
	signature = dbus_message_iter_get_signature(&iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != a{oa{sv}}", signature);

fail:
	if (_pcms != NULL)
		free(_pcms);
	rv = FALSE;

success:
	if (rep != NULL)
		dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}

dbus_bool_t bluealsa_dbus_get_pcm(
		struct ba_dbus_ctx *ctx,
		const bdaddr_t *addr,
		unsigned int flags,
		struct ba_pcm *pcm,
		DBusError *error) {

	struct ba_pcm *pcms = NULL;
	dbus_bool_t rv = TRUE;
	size_t length = 0;
	size_t i;

	if (!bluealsa_dbus_get_pcms(ctx, &pcms, &length, error))
		return FALSE;

	for (i = 0; i < length; i++)
		if (bacmp(&pcms[i].addr, addr) == 0 &&
				(pcms[i].flags & flags) == flags) {
			memcpy(pcm, &pcms[i], sizeof(*pcm));
			goto final;
		}

	rv = FALSE;
	dbus_set_error(error, DBUS_ERROR_FILE_NOT_FOUND, "PCM not found");

final:
	free(pcms);
	return rv;
}

dbus_bool_t bluealsa_dbus_pcm_open(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		int operation_mode,
		int *fd_pcm,
		int *fd_pcm_ctrl,
		DBusError *error) {

	const char *mode = NULL;
	if (operation_mode == BA_PCM_FLAG_SOURCE)
		mode = "source";
	else if (operation_mode == BA_PCM_FLAG_SINK)
		mode = "sink";

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, pcm_path,
					BLUEALSA_INTERFACE_PCM, "Open")) == NULL) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID)) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
		dbus_message_unref(msg);
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
			DBUS_TYPE_UNIX_FD, fd_pcm,
			DBUS_TYPE_UNIX_FD, fd_pcm_ctrl,
			DBUS_TYPE_INVALID);

	dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}

dbus_bool_t bluealsa_dbus_rfcomm_open(
		struct ba_dbus_ctx *ctx,
		const char *rfcomm_path,
		int *fd_rfcomm,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, rfcomm_path,
					BLUEALSA_INTERFACE_RFCOMM, "Open")) == NULL) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
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

dbus_bool_t bluealsa_dbus_pcm_ctrl_send(
		int fd_pcm_ctrl,
		const char *command,
		DBusError *error) {

	ssize_t len = strlen(command);
	if (write(fd_pcm_ctrl, command, len) == -1) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Write: %s", strerror(errno));
		return FALSE;
	}

	/* PCM controller socket is created in the non-blocking
	 * mode, so we have to poll for reading by ourself. */
	struct pollfd pfd = { fd_pcm_ctrl, POLLIN, 0 };
	poll(&pfd, 1, -1);

	char rep[32];
	if ((len = read(fd_pcm_ctrl, rep, sizeof(rep))) == -1) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Read: %s", strerror(errno));
		return FALSE;
	}

	if (strncmp(rep, "OK", len) != 0) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Response: %s", rep);
		errno = ENOMSG;
		return FALSE;
	}

	return TRUE;
}

/**
 * Call the given function for each key/value pairs. */
dbus_bool_t bluealsa_dbus_message_iter_dict(
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
		DBusMessageIter iter_entry_val;
		const char *key;

		if (dbus_message_iter_get_arg_type(&iter_dict) != DBUS_TYPE_DICT_ENTRY)
			goto fail;
		dbus_message_iter_recurse(&iter_dict, &iter_entry);
		if (dbus_message_iter_get_arg_type(&iter_entry) != DBUS_TYPE_STRING)
			goto fail;
		dbus_message_iter_get_basic(&iter_entry, &key);
		if (!dbus_message_iter_next(&iter_entry) ||
				dbus_message_iter_get_arg_type(&iter_entry) != DBUS_TYPE_VARIANT)
			goto fail;
		dbus_message_iter_recurse(&iter_entry, &iter_entry_val);

		if (!cb(key, &iter_entry_val, userdata, error))
			return FALSE;

	}

	return TRUE;

fail:
	signature = dbus_message_iter_get_signature(iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != a{sv}", signature);
	dbus_free(signature);
	return FALSE;
}

/**
 * Parse BlueALSA PCM. */
dbus_bool_t bluealsa_dbus_message_iter_get_pcm(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm) {

	const char *path;
	char *signature;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_OBJECT_PATH)
		goto fail;

	memset(pcm, 0, sizeof(*pcm));

	dbus_message_iter_get_basic(iter, &path);
	strncpy(pcm->pcm_path, path, sizeof(pcm->pcm_path) - 1);

	if (strstr(path, "/a2dp") != NULL)
		pcm->flags |= BA_PCM_FLAG_PROFILE_A2DP;
	if (strstr(path, "/sco") != NULL)
		pcm->flags |= BA_PCM_FLAG_PROFILE_SCO;

	if (!dbus_message_iter_next(iter))
		goto fail;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_message_iter_get_pcm_props(iter, &err, pcm)) {
		dbus_set_error(error, err.name, "Get properties: %s", err.message);
		dbus_error_free(&err);
		return FALSE;
	}

	return TRUE;

fail:
	signature = dbus_message_iter_get_signature(iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != oa{sv}", signature);
	dbus_free(signature);
	return FALSE;
}

/**
 * Callback function for BlueALSA PCM properties parser. */
static dbus_bool_t bluealsa_dbus_message_iter_get_pcm_props_cb(const char *key,
		DBusMessageIter *variant, void *userdata, DBusError *error) {
	struct ba_pcm *pcm = (struct ba_pcm *)userdata;

	char type = dbus_message_iter_get_arg_type(variant);
	char type_expected;
	const char *tmp;

	if (strcmp(key, "Device") == 0) {
		if (type != (type_expected = DBUS_TYPE_OBJECT_PATH))
			goto fail;
		dbus_message_iter_get_basic(variant, &tmp);
		strncpy(pcm->device_path, tmp, sizeof(pcm->device_path) - 1);
		path2ba(tmp, &pcm->addr);
	}
	else if (strcmp(key, "Modes") == 0) {
		if (type != DBUS_TYPE_ARRAY &&
				dbus_message_iter_get_element_type(variant) != DBUS_TYPE_STRING) {
			dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
					"Incorrect variant for '%s': %c%c != as", key,
					type, dbus_message_iter_get_element_type(variant));
			return FALSE;
		}
		DBusMessageIter iter_props_modes;
		for (dbus_message_iter_recurse(variant, &iter_props_modes);
				dbus_message_iter_get_arg_type(&iter_props_modes) != DBUS_TYPE_INVALID;
				dbus_message_iter_next(&iter_props_modes)) {
			dbus_message_iter_get_basic(&iter_props_modes, &tmp);
			if (strcmp(tmp, "source") == 0)
				pcm->flags |= BA_PCM_FLAG_SOURCE;
			else if (strcmp(tmp, "sink") == 0)
				pcm->flags |= BA_PCM_FLAG_SINK;
		}
	}
	else if (strcmp(key, "Format") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->format);
	}
	else if (strcmp(key, "Channels") == 0) {
		if (type != (type_expected = DBUS_TYPE_BYTE))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->channels);
	}
	else if (strcmp(key, "Sampling") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT32))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->sampling);
	}
	else if (strcmp(key, "Codec") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->codec);
	}
	else if (strcmp(key, "Delay") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->delay);
	}
	else if (strcmp(key, "Volume") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->volume.raw);
	}

	return TRUE;

fail:
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;
}

/**
 * Parse BlueALSA PCM properties. */
dbus_bool_t bluealsa_dbus_message_iter_get_pcm_props(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm) {
	return bluealsa_dbus_message_iter_dict(iter, error,
			bluealsa_dbus_message_iter_get_pcm_props_cb, pcm);
}
