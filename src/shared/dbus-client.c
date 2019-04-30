/*
 * BlueALSA - dbus-client.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
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
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, strerror(ENOMEM));
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
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, strerror(ENOMEM));
		return FALSE;
	}

	dbus_bool_t rv = TRUE;
	struct ba_pcm *_pcms = NULL;
	size_t i;

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter))
		goto fail_signature;

	DBusMessageIter iter_pcms;
	for (dbus_message_iter_recurse(&iter, &iter_pcms), i = 0;
			dbus_message_iter_get_arg_type(&iter_pcms) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_pcms), i++) {

		if (dbus_message_iter_get_arg_type(&iter_pcms) != DBUS_TYPE_DICT_ENTRY)
			goto fail_signature;

		struct ba_pcm *tmp = _pcms;
		if ((tmp = realloc(tmp, (i + 1) * sizeof(*tmp))) == NULL) {
			dbus_set_error(error, DBUS_ERROR_NO_MEMORY, "%s", strerror(ENOMEM));
			goto fail;
		}

		_pcms = tmp;

		DBusMessageIter iter_pcms_entry;
		dbus_message_iter_recurse(&iter_pcms, &iter_pcms_entry);

		if (!bluealsa_dbus_message_iter_get_pcm(&iter_pcms_entry, NULL, &_pcms[i]))
			goto fail_signature;

	}

	*pcms = _pcms;
	*length = i;

	goto success;

fail_signature:
	dbus_set_error_const(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Server returned incorrect message");

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
	size_t length = 0;
	size_t i;

	if (!bluealsa_dbus_get_pcms(ctx, &pcms, &length, error))
		return FALSE;

	for (i = 0; i < length; i++)
		if (bacmp(&pcms[i].addr, addr) == 0 &&
				(pcms[i].flags & flags) == flags) {
			memcpy(pcm, &pcms[i], sizeof(*pcm));
			free(pcms);
			return TRUE;
		}

	free(pcms);
	dbus_set_error_const(error, DBUS_ERROR_FILE_NOT_FOUND, "PCM not found");
	return FALSE;
}

dbus_bool_t bluealsa_dbus_pcm_open(
		struct ba_dbus_ctx *ctx,
		const struct ba_pcm *pcm,
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
	if ((msg = dbus_message_new_method_call(ctx->ba_service, pcm->pcm_path,
					BLUEALSA_INTERFACE_PCM, "Open")) == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, strerror(ENOMEM));
		return FALSE;
	}

	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID)) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, strerror(ENOMEM));
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

dbus_bool_t bluealsa_dbus_pcm_ctrl_send(
		int fd_pcm_ctrl,
		const char *command,
		DBusError *error) {

	ssize_t len = strlen(command);
	if (write(fd_pcm_ctrl, command, len) == -1) {
		dbus_set_error_const(error, DBUS_ERROR_FAILED, strerror(errno));
		return FALSE;
	}

	/* PCM controller socket is created in the non-blocking
	 * mode, so we have to poll for reading by ourself. */
	struct pollfd pfd = { fd_pcm_ctrl, POLLIN, 0 };
	poll(&pfd, 1, -1);

	char rep[32];
	if ((len = read(fd_pcm_ctrl, rep, sizeof(rep))) == -1) {
		dbus_set_error_const(error, DBUS_ERROR_FAILED, strerror(errno));
		return FALSE;
	}

	if (strncmp(rep, "OK", len) != 0) {
		dbus_set_error_const(error, DBUS_ERROR_FAILED, rep);
		errno = ENOMSG;
		return FALSE;
	}

	return TRUE;
}

dbus_bool_t bluealsa_dbus_message_iter_get_pcm(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm) {

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_OBJECT_PATH)
		goto fail_signature;

	const char *path;
	dbus_message_iter_get_basic(iter, &path);

	memset(pcm, 0, sizeof(*pcm));
	strncpy(pcm->pcm_path, path, sizeof(pcm->pcm_path) - 1);

	if (strstr(path, "/a2dp") != NULL)
		pcm->flags |= BA_PCM_FLAG_PROFILE_A2DP;
	if (strstr(path, "/sco") != NULL)
		pcm->flags |= BA_PCM_FLAG_PROFILE_SCO;

	if (!dbus_message_iter_next(iter) ||
			dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		goto fail_signature;

	DBusMessageIter iter_props;
	for (dbus_message_iter_recurse(iter, &iter_props);
			dbus_message_iter_get_arg_type(&iter_props) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_props)) {

		DBusMessageIter iter_props_entry;
		DBusMessageIter iter_props_entry_val;
		const char *key;

		dbus_message_iter_recurse(&iter_props, &iter_props_entry);
		dbus_message_iter_get_basic(&iter_props_entry, &key);
		dbus_message_iter_next(&iter_props_entry);
		dbus_message_iter_recurse(&iter_props_entry, &iter_props_entry_val);

		if (strcmp(key, "Device") == 0) {
			const char *path;
			dbus_message_iter_get_basic(&iter_props_entry_val, &path);
			strncpy(pcm->device_path, path, sizeof(pcm->device_path) - 1);
			path2ba(path, &pcm->addr);
		}
		else if (strcmp(key, "Modes") == 0) {
			DBusMessageIter iter_props_modes;
			for (dbus_message_iter_recurse(&iter_props_entry_val, &iter_props_modes);
					dbus_message_iter_get_arg_type(&iter_props_modes) != DBUS_TYPE_INVALID;
					dbus_message_iter_next(&iter_props_modes)) {
				const char *mode;
				dbus_message_iter_get_basic(&iter_props_modes, &mode);
				if (strcmp(mode, "source") == 0)
					pcm->flags |= BA_PCM_FLAG_SOURCE;
				else if (strcmp(mode, "sink") == 0)
					pcm->flags |= BA_PCM_FLAG_SINK;
			}
		}
		else if (strcmp(key, "Channels") == 0) {
			unsigned char channels;
			dbus_message_iter_get_basic(&iter_props_entry_val, &channels);
			pcm->channels = channels;
		}
		else if (strcmp(key, "Sampling") == 0) {
			dbus_uint32_t sampling;
			dbus_message_iter_get_basic(&iter_props_entry_val, &sampling);
			pcm->sampling = sampling;
		}
		else if (strcmp(key, "Codec") == 0) {
			dbus_uint16_t codec;
			dbus_message_iter_get_basic(&iter_props_entry_val, &codec);
			pcm->codec = codec;
		}
		else if (strcmp(key, "Delay") == 0) {
			dbus_uint16_t delay;
			dbus_message_iter_get_basic(&iter_props_entry_val, &delay);
			pcm->delay = delay;
		}

	}

	return TRUE;

fail_signature:
	dbus_set_error_const(error, DBUS_ERROR_INVALID_SIGNATURE, "Incorrect message");
	return FALSE;
}
