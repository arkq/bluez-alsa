/*
 * BlueALSA - dbus-client-pcm.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "dbus-client-pcm.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <unistd.h>

#include "a2dp-codecs.h"
#include "defs.h"

static int path2ba(const char *path, bdaddr_t *ba) {

	unsigned int x[6];
	if ((path = strstr(path, "/dev_")) == NULL ||
			sscanf(&path[5], "%x_%x_%x_%x_%x_%x",
				&x[5], &x[4], &x[3], &x[2], &x[1], &x[0]) != 6)
		return -1;

	for (size_t i = 0; i < 6; i++)
		ba->b[i] = x[i];

	return 0;
}

dbus_bool_t ba_dbus_pcm_get_all(
		struct ba_dbus_ctx *ctx,
		struct ba_pcm **pcms,
		size_t *length,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, "/org/bluealsa",
					DBUS_INTERFACE_OBJECT_MANAGER, "GetManagedObjects")) == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	dbus_bool_t rv = TRUE;
	struct ba_pcm *_pcms = NULL;
	size_t _length = 0;

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	DBusMessageIter iter_objects;
	for (dbus_message_iter_recurse(&iter, &iter_objects);
			dbus_message_iter_get_arg_type(&iter_objects) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_objects)) {

		if (dbus_message_iter_get_arg_type(&iter_objects) != DBUS_TYPE_DICT_ENTRY) {
			char *signature = dbus_message_iter_get_signature(&iter);
			dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
					"Incorrect signature: %s != a{oa{sa{sv}}}", signature);
			dbus_free(signature);
			goto fail;
		}

		DBusMessageIter iter_object_entry;
		dbus_message_iter_recurse(&iter_objects, &iter_object_entry);

		struct ba_pcm pcm;
		DBusError err = DBUS_ERROR_INIT;
		if (!dbus_message_iter_get_ba_pcm(&iter_object_entry, &err, &pcm)) {
			dbus_set_error(error, err.name, "Get PCM: %s", err.message);
			dbus_error_free(&err);
			goto fail;
		}

		if (pcm.transport == BA_PCM_TRANSPORT_NONE)
			continue;

		struct ba_pcm *tmp = _pcms;
		if ((tmp = realloc(tmp, (_length + 1) * sizeof(*tmp))) == NULL) {
			dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
			goto fail;
		}

		_pcms = tmp;

		memcpy(&_pcms[_length++], &pcm, sizeof(*_pcms));

	}

	*pcms = _pcms;
	*length = _length;

	goto success;

fail:
	free(_pcms);
	rv = FALSE;

success:
	if (rep != NULL)
		dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}

dbus_bool_t ba_dbus_pcm_get(
		struct ba_dbus_ctx *ctx,
		const bdaddr_t *addr,
		unsigned int transports,
		unsigned int mode,
		struct ba_pcm *pcm,
		DBusError *error) {

	const bool get_last = bacmp(addr, BDADDR_ANY) == 0;
	struct ba_pcm *pcms = NULL;
	struct ba_pcm *match = NULL;
	dbus_bool_t rv = TRUE;
	size_t length = 0;
	uint32_t seq = 0;

	if (!ba_dbus_pcm_get_all(ctx, &pcms, &length, error))
		return FALSE;

	for (size_t i = 0; i < length; i++) {
		if (get_last) {
			if (pcms[i].sequence >= seq &&
					pcms[i].transport & transports &&
					pcms[i].mode == mode) {
				seq = pcms[i].sequence;
				match = &pcms[i];
			}
		}
		else if (bacmp(&pcms[i].addr, addr) == 0 &&
				pcms[i].transport & transports &&
				pcms[i].mode == mode) {
			match = &pcms[i];
			break;
		}
	}

	if (match != NULL)
		memcpy(pcm, match, sizeof(*pcm));
	else {
		dbus_set_error(error, DBUS_ERROR_FILE_NOT_FOUND, "PCM not found");
		rv = FALSE;
	}

	free(pcms);
	return rv;
}

/**
 * Open BlueALSA PCM stream. */
dbus_bool_t ba_dbus_pcm_open(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		int *fd_pcm,
		int *fd_pcm_ctrl,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, pcm_path,
					BLUEALSA_INTERFACE_PCM, "Open")) == NULL) {
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
			DBUS_TYPE_UNIX_FD, fd_pcm,
			DBUS_TYPE_UNIX_FD, fd_pcm_ctrl,
			DBUS_TYPE_INVALID);

	dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}

const char *ba_dbus_pcm_codec_get_canonical_name(
		const char *alias) {

	static const char *sco_codecs[] = { "CVSD", "mSBC", "LC3-SWB" };
	for (size_t i = 0; i < ARRAYSIZE(sco_codecs); i++)
		if (strcasecmp(sco_codecs[i], alias) == 0)
			return sco_codecs[i];

	return a2dp_codecs_get_canonical_name(alias);
}

static void dbus_message_iter_get_codec_data(
		DBusMessageIter * restrict variant,
		struct ba_pcm_codec * restrict codec) {

	DBusMessageIter iter;
	unsigned char *data;
	int len;

	dbus_message_iter_recurse(variant, &iter);
	dbus_message_iter_get_fixed_array(&iter, &data, &len);

	codec->data_len = MIN((size_t)len, ARRAYSIZE(codec->data));
	memcpy(codec->data, data, codec->data_len);

}

static void dbus_message_iter_get_codec_channels(
		DBusMessageIter * restrict variant,
		struct ba_pcm_codec * restrict codec) {

	DBusMessageIter iter;
	unsigned char *data;
	int len;

	dbus_message_iter_recurse(variant, &iter);
	dbus_message_iter_get_fixed_array(&iter, &data, &len);

	len = MIN(len, ARRAYSIZE(codec->channels));
	for (size_t i = 0; i < (size_t)len; i++)
		codec->channels[i] = data[i];

}

static void dbus_message_iter_get_codec_rates(
		DBusMessageIter * restrict variant,
		struct ba_pcm_codec * restrict codec) {

	DBusMessageIter iter;
	dbus_uint32_t *data;
	int len;

	dbus_message_iter_recurse(variant, &iter);
	dbus_message_iter_get_fixed_array(&iter, &data, &len);

	len = MIN(len, ARRAYSIZE(codec->rates));
	for (size_t i = 0; i < (size_t)len; i++)
		codec->rates[i] = data[i];

}

static void dbus_message_iter_get_codec_channel_maps(
		DBusMessageIter * restrict variant,
		struct ba_pcm_codec * restrict codec) {

	size_t i;
	DBusMessageIter iter_array;
	for (dbus_message_iter_recurse(variant, &iter_array), i = 0;
			dbus_message_iter_get_arg_type(&iter_array) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_array)) {

		const char *data[ARRAYSIZE(*codec->channel_maps)];
		size_t length = ARRAYSIZE(data);

		dbus_message_iter_array_get_strings(&iter_array, NULL, data, &length);

		for (size_t j = 0; j < length; j++)
			strncpy(codec->channel_maps[i][j], data[j], sizeof(codec->channel_maps[i][j]) - 1);

		i++;
	}

}

/**
 * Callback function for BlueALSA PCM codec props parser. */
static dbus_bool_t ba_dbus_message_iter_pcm_codec_get_props_cb(const char *key,
		DBusMessageIter *value, void *userdata, DBusError *error) {
	struct ba_pcm_codec *codec = (struct ba_pcm_codec *)userdata;

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

	if (strcmp(key, "Capabilities") == 0) {
		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;
		dbus_message_iter_get_codec_data(&variant, codec);
	}
	else if (strcmp(key, "Channels") == 0) {
		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;
		dbus_message_iter_get_codec_channels(&variant, codec);
	}
	else if (strcmp(key, "ChannelMaps") == 0) {
		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;
		dbus_message_iter_get_codec_channel_maps(&variant, codec);
	}
	else if (strcmp(key, "Rates") == 0) {
		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;
		dbus_message_iter_get_codec_rates(&variant, codec);
	}

	return TRUE;

fail:
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;
}

/**
 * Callback function for BlueALSA PCM codec list parser. */
static dbus_bool_t ba_dbus_message_iter_pcm_codecs_get_cb(const char *key,
		DBusMessageIter *value, void *userdata, DBusError *error) {

	struct ba_pcm_codecs *codecs = (struct ba_pcm_codecs *)userdata;
	const size_t len = codecs->codecs_len;

	struct ba_pcm_codec *tmp = codecs->codecs;
	if ((tmp = realloc(tmp, (len + 1) * sizeof(*tmp))) == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	struct ba_pcm_codec *codec = &tmp[len];
	codecs->codecs = tmp;

	memset(codec, 0, sizeof(*codec));
	strncpy(codec->name, key, sizeof(codec->name));
	codec->name[sizeof(codec->name) - 1] = '\0';

	if (!dbus_message_iter_dict(value, error,
				ba_dbus_message_iter_pcm_codec_get_props_cb, codec))
		return FALSE;

	codecs->codecs_len = len + 1;
	return TRUE;
}

/**
 * Get BlueALSA PCM Bluetooth audio codecs. */
dbus_bool_t ba_dbus_pcm_codecs_get(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		struct ba_pcm_codecs *codecs,
		DBusError *error) {

	DBusMessage *msg = NULL, *rep = NULL;
	dbus_bool_t rv = FALSE;

	if ((msg = dbus_message_new_method_call(ctx->ba_service, pcm_path,
					BLUEALSA_INTERFACE_PCM, "GetCodecs")) == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	codecs->codecs = NULL;
	codecs->codecs_len = 0;

	if (!dbus_message_iter_dict(&iter, error,
				ba_dbus_message_iter_pcm_codecs_get_cb, codecs)) {
		free(codecs->codecs);
		goto fail;
	}

	rv = TRUE;

fail:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return rv;
}

/**
 * Free BlueALSA PCM codecs structure. */
void ba_dbus_pcm_codecs_free(
		struct ba_pcm_codecs *codecs) {
	free(codecs->codecs);
	codecs->codecs = NULL;
}

/**
 * Select BlueALSA PCM Bluetooth audio codec. */
dbus_bool_t ba_dbus_pcm_select_codec(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		const char *codec,
		const void *configuration,
		size_t configuration_len,
		unsigned int channels,
		unsigned int rate,
		unsigned int flags,
		DBusError *error) {

	DBusMessage *msg = NULL, *rep = NULL;
	dbus_bool_t rv = FALSE;

	if ((msg = dbus_message_new_method_call(ctx->ba_service, pcm_path,
					BLUEALSA_INTERFACE_PCM, "SelectCodec")) == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	DBusMessageIter iter;
	DBusMessageIter props;

	dbus_message_iter_init_append(msg, &iter);
	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &codec) ||
			!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &props)) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	if (configuration != NULL &&
			configuration_len > 0) {
		const char *property = "Configuration";
		DBusMessageIter dict;
		DBusMessageIter config;
		DBusMessageIter array;
		if (!dbus_message_iter_open_container(&props, DBUS_TYPE_DICT_ENTRY, NULL, &dict) ||
				!dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &property) ||
				!dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "ay", &config) ||
				!dbus_message_iter_open_container(&config, DBUS_TYPE_ARRAY, "y", &array) ||
				!dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE, &configuration, configuration_len) ||
				!dbus_message_iter_close_container(&config, &array) ||
				!dbus_message_iter_close_container(&dict, &config) ||
				!dbus_message_iter_close_container(&props, &dict)) {
			dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
			goto fail;
		}
	}

	if (channels != 0) {
		const uint8_t value = channels;
		if (!dbus_message_iter_dict_append_basic(&props, "Channels", DBUS_TYPE_BYTE, &value)) {
			dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
			goto fail;
		}
	}

	if (rate != 0) {
		const uint32_t value = rate;
		if (!dbus_message_iter_dict_append_basic(&props, "Rate", DBUS_TYPE_UINT32, &value)) {
			dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
			goto fail;
		}
	}

	if (flags & BA_PCM_SELECT_CODEC_FLAG_NON_CONFORMANT) {
		const dbus_bool_t value = TRUE;
		if (!dbus_message_iter_dict_append_basic(&props, "NonConformant", DBUS_TYPE_BOOLEAN, &value)) {
			dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
			goto fail;
		}
	}

	if (!dbus_message_iter_close_container(&iter, &props)) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	rv = TRUE;

fail:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return rv;
}

/**
 * Update BlueALSA PCM property. */
dbus_bool_t ba_dbus_pcm_update(
		struct ba_dbus_ctx *ctx,
		const struct ba_pcm *pcm,
		enum ba_pcm_property property,
		DBusError *error) {

	static const char *interface = BLUEALSA_INTERFACE_PCM;
	const char *_property = NULL;
	const char *type = NULL;

	switch (property) {
	case BLUEALSA_PCM_CLIENT_DELAY:
		_property = "ClientDelay";
		type = DBUS_TYPE_INT16_AS_STRING;
		break;
	case BLUEALSA_PCM_SOFT_VOLUME:
		_property = "SoftVolume";
		type = DBUS_TYPE_BOOLEAN_AS_STRING;
		break;
	case BLUEALSA_PCM_VOLUME:
		_property = "Volume";
		type = DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING;
		break;
	}

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, pcm->pcm_path,
					DBUS_INTERFACE_PROPERTIES, "Set")) == NULL)
		goto fail;

	DBusMessageIter iter;
	DBusMessageIter variant;

	dbus_message_iter_init_append(msg, &iter);
	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface) ||
			!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &_property) ||
			!dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, type, &variant))
		goto fail;

	switch (property) {
	case BLUEALSA_PCM_CLIENT_DELAY:
		if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT16, &pcm->client_delay))
			goto fail;
		break;
	case BLUEALSA_PCM_SOFT_VOLUME:
		if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &pcm->soft_volume))
			goto fail;
		break;
	case BLUEALSA_PCM_VOLUME: {
		DBusMessageIter array;
		const void *volume = &pcm->volume;
		if (!dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &array) ||
				!dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE, &volume, pcm->channels) ||
				!dbus_message_iter_close_container(&variant, &array))
			goto fail;
	} break;
	}

	if (!dbus_message_iter_close_container(&iter, &variant))
		goto fail;

	if (!dbus_connection_send(ctx->conn, msg, NULL))
		goto fail;

	dbus_message_unref(msg);
	return TRUE;

fail:
	if (msg != NULL)
		dbus_message_unref(msg);
	dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY, NULL);
	return FALSE;
}

/**
 * Send command to the BlueALSA PCM controller socket. */
dbus_bool_t ba_dbus_pcm_ctrl_send(
		int fd_pcm_ctrl,
		const char *command,
		int timeout,
		DBusError *error) {

	ssize_t len = strlen(command);
	if (send(fd_pcm_ctrl, command, len, MSG_NOSIGNAL) == -1) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Send: %s", strerror(errno));
		return FALSE;
	}

	/* PCM controller socket is created in the non-blocking
	 * mode, so we have to poll for reading by ourself. If interrupted we
	 * cannot report error EINTR here because the command has already been
	 * sent; so we must wait for the response or else this is a fatal error. */
	struct pollfd pfd = { fd_pcm_ctrl, POLLIN, 0 };
	int res;
	while ((res = poll(&pfd, 1, timeout)) == -1 && errno == EINTR)
		continue;

	if (res == 0) {
		/* poll() timeout - the server has stopped responding to commands */
		errno = EIO;
		dbus_set_error(error, DBUS_ERROR_IO_ERROR, "Read: %s", strerror(errno));
		return FALSE;
	}

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
 * Parse BlueALSA PCM. */
dbus_bool_t dbus_message_iter_get_ba_pcm(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm) {

	const char *path;
	char *signature;

	memset(pcm, 0, sizeof(*pcm));

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_OBJECT_PATH)
		goto fail;
	dbus_message_iter_get_basic(iter, &path);

	if (!dbus_message_iter_next(iter))
		goto fail;

	DBusMessageIter iter_ifaces;
	for (dbus_message_iter_recurse(iter, &iter_ifaces);
			dbus_message_iter_get_arg_type(&iter_ifaces) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_ifaces)) {

		if (dbus_message_iter_get_arg_type(&iter_ifaces) != DBUS_TYPE_DICT_ENTRY)
			goto fail;

		DBusMessageIter iter_iface_entry;
		dbus_message_iter_recurse(&iter_ifaces, &iter_iface_entry);

		const char *iface_name;
		if (dbus_message_iter_get_arg_type(&iter_iface_entry) != DBUS_TYPE_STRING)
			goto fail;
		dbus_message_iter_get_basic(&iter_iface_entry, &iface_name);

		if (strcmp(iface_name, BLUEALSA_INTERFACE_PCM) == 0) {

			strncpy(pcm->pcm_path, path, sizeof(pcm->pcm_path) - 1);

			if (!dbus_message_iter_next(&iter_iface_entry))
				goto fail;

			DBusError err = DBUS_ERROR_INIT;
			if (!dbus_message_iter_get_ba_pcm_props(&iter_iface_entry, &err, pcm)) {
				dbus_set_error(error, err.name, "Get properties: %s", err.message);
				dbus_error_free(&err);
				return FALSE;
			}

			break;
		}

	}

	return TRUE;

fail:
	signature = dbus_message_iter_get_signature(iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != oa{sa{sv}}", signature);
	dbus_free(signature);
	return FALSE;
}

/**
 * Callback function for BlueALSA PCM properties parser. */
static dbus_bool_t dbus_message_iter_get_ba_pcm_props_cb(const char *key,
		DBusMessageIter *value, void *userdata, DBusError *error) {
	struct ba_pcm *pcm = (struct ba_pcm *)userdata;

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
	const char *tmp;

	if (strcmp(key, "Device") == 0) {
		if (type != (type_expected = DBUS_TYPE_OBJECT_PATH))
			goto fail;
		dbus_message_iter_get_basic(&variant, &tmp);
		strncpy(pcm->device_path, tmp, sizeof(pcm->device_path) - 1);
		path2ba(tmp, &pcm->addr);
	}
	else if (strcmp(key, "Sequence") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT32))
			goto fail;
		dbus_message_iter_get_basic(&variant, &pcm->sequence);
	}
	else if (strcmp(key, "Transport") == 0) {
		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;
		dbus_message_iter_get_basic(&variant, &tmp);
		if (strstr(tmp, "A2DP-source") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_A2DP_SOURCE;
		else if (strstr(tmp, "A2DP-sink") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_A2DP_SINK;
		else if (strstr(tmp, "HFP-AG") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_HFP_AG;
		else if (strstr(tmp, "HFP-HF") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_HFP_HF;
		else if (strstr(tmp, "HSP-AG") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_HSP_AG;
		else if (strstr(tmp, "HSP-HS") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_HSP_HS;
	}
	else if (strcmp(key, "Mode") == 0) {
		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;
		dbus_message_iter_get_basic(&variant, &tmp);
		if (strcmp(tmp, "source") == 0)
			pcm->mode = BA_PCM_MODE_SOURCE;
		else if (strcmp(tmp, "sink") == 0)
			pcm->mode = BA_PCM_MODE_SINK;
	}
	else if (strcmp(key, "Running") == 0) {
		if (type != (type_expected = DBUS_TYPE_BOOLEAN))
			goto fail;
		dbus_message_iter_get_basic(&variant, &pcm->running);
	}
	else if (strcmp(key, "Format") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(&variant, &pcm->format);
	}
	else if (strcmp(key, "Channels") == 0) {
		if (type != (type_expected = DBUS_TYPE_BYTE))
			goto fail;
		dbus_message_iter_get_basic(&variant, &pcm->channels);
		pcm->codec.channels[0] = pcm->channels;
	}
	else if (strcmp(key, "ChannelMap") == 0) {
		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;

		const char *data[ARRAYSIZE(pcm->channel_map)];
		size_t length = ARRAYSIZE(data);

		if (!dbus_message_iter_array_get_strings(&variant, error, data, &length))
			return FALSE;

		for (size_t i = 0; i < length; i++)
			strncpy(pcm->channel_map[i], data[i], sizeof(pcm->channel_map[i]) - 1);

	}
	else if (strcmp(key, "Rate") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT32))
			goto fail;
		dbus_message_iter_get_basic(&variant, &pcm->rate);
		pcm->codec.rates[0] = pcm->rate;
	}
	else if (strcmp(key, "Codec") == 0) {
		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;
		dbus_message_iter_get_basic(&variant, &tmp);
		strncpy(pcm->codec.name, tmp, sizeof(pcm->codec.name) - 1);
	}
	else if (strcmp(key, "CodecConfiguration") == 0) {
		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;
		dbus_message_iter_get_codec_data(&variant, &pcm->codec);
	}
	else if (strcmp(key, "Delay") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(&variant, &pcm->delay);
	}
	else if (strcmp(key, "ClientDelay") == 0) {
		if (type != (type_expected = DBUS_TYPE_INT16))
			goto fail;
		dbus_message_iter_get_basic(&variant, &pcm->client_delay);
	}
	else if (strcmp(key, "SoftVolume") == 0) {
		if (type != (type_expected = DBUS_TYPE_BOOLEAN))
			goto fail;
		dbus_message_iter_get_basic(&variant, &pcm->soft_volume);
	}
	else if (strcmp(key, "Volume") == 0) {
		if (type != (type_expected = DBUS_TYPE_ARRAY))
			goto fail;

		DBusMessageIter iter;
		uint8_t *data;
		int len;

		dbus_message_iter_recurse(&variant, &iter);
		dbus_message_iter_get_fixed_array(&iter, &data, &len);

		memcpy(pcm->volume, data, MIN(len, ARRAYSIZE(pcm->volume)));

	}

	return TRUE;

fail:
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;
}

/**
 * Parse BlueALSA PCM properties. */
dbus_bool_t dbus_message_iter_get_ba_pcm_props(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm) {
	return dbus_message_iter_dict(iter, error,
			dbus_message_iter_get_ba_pcm_props_cb, pcm);
}
