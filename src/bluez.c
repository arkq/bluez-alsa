/*
 * bluealsa - bluez.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluez.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2dp-codecs.h"
#include "device.h"
#include "log.h"
#include "transport.h"
#include "utils.h"


static DBusMessage *bluez_error_invalid_arguments(DBusMessage *reply_to, const char *message) {
	return dbus_message_new_error(reply_to, "org.bluez.Error.InvalidArguments", message);
}

static DBusMessage *bluez_error_not_supported(DBusMessage *reply_to, const char *message) {
	return dbus_message_new_error(reply_to, "org.bluez.Error.NotSupported", message);
}

static DBusMessage *bluez_error_failed(DBusMessage *reply_to, const char *message) {
	return dbus_message_new_error(reply_to, "org.bluez.Error.Failed", message);
}

static DBusMessage *bluez_endpoint_select_configuration(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;
	(void)userdata;

	DBusMessage *rep;
	DBusError err;
	a2dp_sbc_t a2dp_sbc = { 0 }, *cap;
	int size;

	dbus_error_init(&err);

	if (!dbus_message_get_args(msg, &err,
				DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &cap, &size,
				DBUS_TYPE_INVALID)) {
		error("Invalid request for %s: %s", "SelectConfiguration()", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	if (cap->frequency & SBC_SAMPLING_FREQ_48000)
		a2dp_sbc.frequency = SBC_SAMPLING_FREQ_48000;
	else if (cap->frequency & SBC_SAMPLING_FREQ_44100)
		a2dp_sbc.frequency = SBC_SAMPLING_FREQ_44100;
	else if (cap->frequency & SBC_SAMPLING_FREQ_32000)
		a2dp_sbc.frequency = SBC_SAMPLING_FREQ_32000;
	else if (cap->frequency & SBC_SAMPLING_FREQ_16000)
		a2dp_sbc.frequency = SBC_SAMPLING_FREQ_16000;
	else {
		error("No supported frequencies: %u", cap->frequency);
		goto fail;
	}

	if (cap->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
		a2dp_sbc.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
	else if (cap->channel_mode & SBC_CHANNEL_MODE_STEREO)
		a2dp_sbc.channel_mode = SBC_CHANNEL_MODE_STEREO;
	else if (cap->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
		a2dp_sbc.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
	else if (cap->channel_mode & SBC_CHANNEL_MODE_MONO) {
		a2dp_sbc.channel_mode = SBC_CHANNEL_MODE_MONO;
	} else {
		error("No supported channel modes: %u", cap->channel_mode);
		goto fail;
	}

	if (cap->block_length & SBC_BLOCK_LENGTH_16)
		a2dp_sbc.block_length = SBC_BLOCK_LENGTH_16;
	else if (cap->block_length & SBC_BLOCK_LENGTH_12)
		a2dp_sbc.block_length = SBC_BLOCK_LENGTH_12;
	else if (cap->block_length & SBC_BLOCK_LENGTH_8)
		a2dp_sbc.block_length = SBC_BLOCK_LENGTH_8;
	else if (cap->block_length & SBC_BLOCK_LENGTH_4)
		a2dp_sbc.block_length = SBC_BLOCK_LENGTH_4;
	else {
		error("No supported block lengths: %u", cap->block_length);
		goto fail;
	}

	if (cap->subbands & SBC_SUBBANDS_8)
		a2dp_sbc.subbands = SBC_SUBBANDS_8;
	else if (cap->subbands & SBC_SUBBANDS_4)
		a2dp_sbc.subbands = SBC_SUBBANDS_4;
	else {
		error("No supported subbands: %u", cap->subbands);
		goto fail;
	}

	if (cap->allocation_method & SBC_ALLOCATION_LOUDNESS)
		a2dp_sbc.allocation_method = SBC_ALLOCATION_LOUDNESS;
	else if (cap->allocation_method & SBC_ALLOCATION_SNR)
		a2dp_sbc.allocation_method = SBC_ALLOCATION_SNR;

	int bitpool = a2dp_default_bitpool(a2dp_sbc.frequency, a2dp_sbc.channel_mode);
	a2dp_sbc.min_bitpool = MAX(MIN_BITPOOL, cap->min_bitpool);
	a2dp_sbc.max_bitpool = MIN(bitpool, cap->max_bitpool);

	uint8_t *pconf = (uint8_t *)&a2dp_sbc;
	rep = dbus_message_new_method_return(msg);
	dbus_message_append_args(rep, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &pconf, size, DBUS_TYPE_INVALID);
	return rep;

fail:
	return bluez_error_invalid_arguments(msg, "Unable to select configuration");
}

static DBusMessage *bluez_endpoint_set_configuration(DBusConnection *conn, DBusMessage *msg, void *userdata) {

	static GHashTable *profiles = NULL;
	DBusMessageIter arg_i, element_i;

	GHashTable *devices = (GHashTable *)userdata;
	struct ba_transport *t;
	struct ba_device *d;
	int profile = -1;
	int codec = -1;

	const char *path;
	const char *dev_path = NULL, *uuid = NULL, *state = NULL;
	const uint8_t *config = NULL;
	uint16_t volume = 0;
	int size = 0;

	if (profiles == NULL) {
		/* initialize profiles hash table - used for profile lookup */

		size_t i;
		const struct profile_data {
			const char *uuid;
			const char *endpoint;
			unsigned int profile;
		} data[] = {
			{ BLUETOOTH_UUID_A2DP_SOURCE, BLUEZ_ENDPOINT_A2DP_SBC_SOURCE, TRANSPORT_PROFILE_A2DP_SOURCE },
			{ BLUETOOTH_UUID_A2DP_SINK, BLUEZ_ENDPOINT_A2DP_SBC_SINK, TRANSPORT_PROFILE_A2DP_SINK },
			{ BLUETOOTH_UUID_A2DP_SOURCE, BLUEZ_ENDPOINT_A2DP_MPEG12_SOURCE, TRANSPORT_PROFILE_A2DP_SOURCE },
			{ BLUETOOTH_UUID_A2DP_SINK, BLUEZ_ENDPOINT_A2DP_MPEG12_SINK, TRANSPORT_PROFILE_A2DP_SINK },
			{ BLUETOOTH_UUID_A2DP_SOURCE, BLUEZ_ENDPOINT_A2DP_MPEG24_SOURCE, TRANSPORT_PROFILE_A2DP_SOURCE },
			{ BLUETOOTH_UUID_A2DP_SINK, BLUEZ_ENDPOINT_A2DP_MPEG24_SINK, TRANSPORT_PROFILE_A2DP_SINK },
			{ BLUETOOTH_UUID_A2DP_SOURCE, BLUEZ_ENDPOINT_A2DP_ATRAC_SOURCE, TRANSPORT_PROFILE_A2DP_SOURCE },
			{ BLUETOOTH_UUID_A2DP_SINK, BLUEZ_ENDPOINT_A2DP_ATRAC_SINK, TRANSPORT_PROFILE_A2DP_SINK },
		};

		profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

		for (i = 0; i < sizeof(data) / sizeof(struct profile_data); i++) {
			gchar *key = g_strdup_printf("%s%s", data[i].uuid, data[i].endpoint);
			g_hash_table_insert(profiles, key, GINT_TO_POINTER(data[i].profile));
		}

	}

	if (!dbus_message_iter_init(msg, &arg_i) || !dbus_message_has_signature(msg, "oa{sv}")) {
		error("Invalid signature for %s: %s != %s", "SetConfiguration()",
				dbus_message_get_signature(msg), "oa{sv}");
		goto fail;
	}

	dbus_message_iter_get_basic(&arg_i, &path);

	if (device_transport_lookup(devices, path) != NULL) {
		error("Transport already configured: %s", path);
		goto fail;
	}

	dbus_message_iter_next(&arg_i);

	/* read transport properties */
	for (dbus_message_iter_recurse(&arg_i, &element_i);
			dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY;
			dbus_message_iter_next(&element_i)) {

		const char *key;
		DBusMessageIter value, entry;
		int var;

		dbus_message_iter_recurse(&element_i, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		var = dbus_message_iter_get_arg_type(&value);

		if (strcmp(key, "Device") == 0) {

			if (var != DBUS_TYPE_OBJECT_PATH) {
				error("Invalid argument type for %s: %s != %s", key,
						dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_OBJECT_PATH));
				goto fail;
			}

			dbus_message_iter_get_basic(&value, &dev_path);

		}
		else if (strcmp(key, "UUID") == 0) {

			if (var != DBUS_TYPE_STRING) {
				error("Invalid argument type for %s: %s != %s", key,
						dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_STRING));
				goto fail;
			}

			dbus_message_iter_get_basic(&value, &uuid);

			const char *endpoint_path = dbus_message_get_path(msg);
			gchar *key = g_strdup_printf("%s%s", uuid, endpoint_path);

			profile = GPOINTER_TO_INT(g_hash_table_lookup(profiles, key));
			g_free(key);

			if (profile == 0) {
				error("UUID %s of transport %s incompatible with endpoint %s", uuid, path, endpoint_path);
				goto fail;
			}

		}
		else if (strcmp(key, "Codec") == 0) {

			if (var != DBUS_TYPE_BYTE) {
				error("Invalid argument type for %s: %s != %s", key,
						dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_BYTE));
				goto fail;
			}

			dbus_message_iter_get_basic(&value, &codec);

		}
		else if (strcmp(key, "Configuration") == 0) {

			if (var != DBUS_TYPE_ARRAY) {
				error("Invalid argument type for %s: %s != %s", key,
						dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_ARRAY));
				goto fail;
			}

			DBusMessageIter array;
			a2dp_sbc_t *c;

			dbus_message_iter_recurse(&value, &array);
			if ((var = dbus_message_iter_get_arg_type(&array)) != DBUS_TYPE_BYTE) {
				error("Invalid array type for %s: %s != %s", key,
						dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_BYTE));
				goto fail;
			}

			dbus_message_iter_get_fixed_array(&array, &config, &size);
			if (size != sizeof(a2dp_sbc_t)) {
				error("Invalid configuration: %s:", "Invalid size");
				goto fail;
			}

			c = (a2dp_sbc_t *)config;

			if (c->frequency != SBC_SAMPLING_FREQ_16000 && c->frequency != SBC_SAMPLING_FREQ_32000 &&
					c->frequency != SBC_SAMPLING_FREQ_44100 && c->frequency != SBC_SAMPLING_FREQ_48000) {
				error("Invalid configuration: %s:", "Invalid sampling frequency");
				goto fail;
			}

			if (c->channel_mode != SBC_CHANNEL_MODE_MONO && c->channel_mode != SBC_CHANNEL_MODE_DUAL_CHANNEL &&
					c->channel_mode != SBC_CHANNEL_MODE_STEREO && c->channel_mode != SBC_CHANNEL_MODE_JOINT_STEREO) {
				error("Invalid configuration: %s:", "Invalid channel mode");
				goto fail;
			}

			if (c->allocation_method != SBC_ALLOCATION_SNR && c->allocation_method != SBC_ALLOCATION_LOUDNESS) {
				error("Invalid configuration: %s:", "Invalid allocation method");
				goto fail;
			}

			if (c->subbands != SBC_SUBBANDS_4 && c->subbands != SBC_SUBBANDS_8) {
				error("Invalid configuration: %s:", "Invalid SBC subbands");
				goto fail;
			}

			if (c->block_length != SBC_BLOCK_LENGTH_4 && c->block_length != SBC_BLOCK_LENGTH_8 &&
					c->block_length != SBC_BLOCK_LENGTH_12 && c->block_length != SBC_BLOCK_LENGTH_16) {
				error("Invalid configuration: %s:", "Invalid block length");
				goto fail;
			}

		}
		else if (strcmp(key, "State") == 0) {

			if (var != DBUS_TYPE_STRING) {
				error("Invalid argument type for %s: %s != %s", key,
						dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_STRING));
				continue;
			}

			dbus_message_iter_get_basic(&value, &state);

		}
		else if (strcmp(key, "Delay") == 0) {
		}
		else if (strcmp(key, "Volume") == 0) {

			if (var != DBUS_TYPE_UINT16) {
				error("Invalid argument type for %s: %s != %s", key,
						dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_STRING));
				continue;
			}

			dbus_message_iter_get_basic(&value, &volume);

			/* scale volume from 0 to 100 */
			volume = volume * 100 / 127;

		}

	}

	if ((d = g_hash_table_lookup(devices, dev_path)) == NULL) {
		bdaddr_t addr;
		dbus_devpath_to_bdaddr(dev_path, &addr);
		/* TODO: Get real device name! */
		d = device_new(&addr, dev_path);
		g_hash_table_insert(devices, g_strdup(dev_path), d);
	}

	/* Create a new transport with a human-readable name. Since the transport
	 * name can not be obtained from the client, we will use a fall-back one. */
	if ((t = transport_new(conn, dbus_message_get_sender(msg), path,
					bluetooth_profile_to_string(profile, codec), profile, codec, config, size)) == NULL) {
		error("Cannot create new transport: %s", strerror(errno));
		goto fail;
	}

	transport_set_state_from_string(t, state);
	t->volume = volume;

	g_hash_table_insert(d->transports, g_strdup(path), t);

	debug("%s configured for device %s", t->name, batostr(&d->addr));
	return dbus_message_new_method_return(msg);

fail:
	return bluez_error_invalid_arguments(msg, "Unable to set configuration");
}

static DBusMessage *bluez_endpoint_clear_configuration(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;

	GHashTable *devices = (GHashTable *)userdata;
	const char *path;
	DBusError err;

	dbus_error_init(&err);

	if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
		error("Invalid request for %s: %s", "ClearConfiguration()", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	device_transport_remove(devices, path);
	return dbus_message_new_method_return(msg);

fail:
	return bluez_error_invalid_arguments(msg, "Unable to clear configuration");
}

static DBusMessage *bluez_endpoint_release(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;
	(void)userdata;
	return dbus_message_new_method_return(msg);
}

static DBusHandlerResult bluez_endpoint_handler(DBusConnection *conn, DBusMessage *msg, void *userdata) {

	const char *path = dbus_message_get_path(msg);
	const char *member = dbus_message_get_member(msg);
	DBusMessage *rep = NULL;

	debug("Endpoint handler: %s/%s()", path, member);

	if (dbus_message_is_method_call(msg, "org.bluez.MediaEndpoint1", "SelectConfiguration"))
		rep = bluez_endpoint_select_configuration(conn, msg, userdata);
	else if (dbus_message_is_method_call(msg, "org.bluez.MediaEndpoint1", "SetConfiguration"))
		rep = bluez_endpoint_set_configuration(conn, msg, userdata);
	else if (dbus_message_is_method_call(msg, "org.bluez.MediaEndpoint1", "ClearConfiguration"))
		rep = bluez_endpoint_clear_configuration(conn, msg, userdata);
	else if (dbus_message_is_method_call(msg, "org.bluez.MediaEndpoint1", "Release"))
		rep = bluez_endpoint_release(conn, msg, userdata);
	else {
		warn("Unsupported endpoint method: %s", member);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (rep != NULL) {
		dbus_connection_send(conn, rep, NULL);
		dbus_message_unref(rep);
	}

  return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable endpoint_vtable = {
	.message_function = bluez_endpoint_handler,
};

static DBusMessage *bluez_profile_new_connection(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;
	(void)userdata;
	return bluez_error_not_supported(msg, "Not implemented yet");
}

static DBusMessage *bluez_profile_request_disconnection(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;
	(void)userdata;
	return bluez_error_not_supported(msg, "Not implemented yet");
}

static DBusMessage *bluez_profile_release(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;
	(void)userdata;
	return bluez_error_not_supported(msg, "Not implemented yet");
}

static DBusHandlerResult bluez_profile_handler(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)userdata;

	const char *path = dbus_message_get_path(msg);
	const char *member = dbus_message_get_member(msg);
	DBusMessage *rep = NULL;

	debug("Profile handler: %s/%s()", path, member);

	if (dbus_message_is_method_call(msg, "org.bluez.Profile1", "NewConnection"))
		rep = bluez_profile_new_connection(conn, msg, userdata);
	else if (dbus_message_is_method_call(msg, "org.bluez.Profile1", "RequestDisconnection"))
		rep = bluez_profile_request_disconnection(conn, msg, userdata);
	else if (dbus_message_is_method_call(msg, "org.bluez.Profile1", "Release"))
		rep = bluez_profile_release(conn, msg, userdata);
	else {
		warn("Unsupported profile method: %s", member);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (rep != NULL) {
		dbus_connection_send(conn, rep, NULL);
		dbus_message_unref(rep);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable profile_vtable = {
	.message_function = bluez_profile_handler,
};

static DBusHandlerResult bluez_signal_handler(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;

	const char *path = dbus_message_get_path(msg);
	GHashTable *devices = (GHashTable *)userdata;

	if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {

		DBusMessageIter arg_i;
		const char *iface;

		if (!dbus_message_iter_init(msg, &arg_i) || !dbus_message_has_signature(msg, "sa{sv}as")) {
			error("Invalid signature for %s: %s != %s", "PropertiesChanged",
					dbus_message_get_signature(msg), "sa{sv}as");
			goto fail;
		}

		dbus_message_iter_get_basic(&arg_i, &iface);
		dbus_message_iter_next(&arg_i);

		if (strcmp(iface, "org.bluez.Adapter1") == 0) {
			debug("Properties changed in adapter %s", path);
		}
		else if (strcmp(iface, "org.bluez.Device1") == 0) {
			debug("Properties changed in device %s", path);
		}
		else if (strcmp(iface, "org.bluez.MediaTransport1") == 0) {
			debug("Properties changed in media transport %s", path);

			DBusMessageIter element_i;
			struct ba_transport *t;

			if ((t = device_transport_lookup(devices, path)) == NULL) {
				error("Transport not available: %s", path);
				goto fail;
			}

			for (dbus_message_iter_recurse(&arg_i, &element_i);
					dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY;
					dbus_message_iter_next(&element_i)) {

				const char *key, *state;
				DBusMessageIter value, dict_i;
				int var;

				dbus_message_iter_recurse(&element_i, &dict_i);
				dbus_message_iter_get_basic(&dict_i, &key);

				dbus_message_iter_next(&dict_i);
				dbus_message_iter_recurse(&dict_i, &value);

				var = dbus_message_iter_get_arg_type(&value);

				if (strcmp(key, "State") == 0) {

					if (var != DBUS_TYPE_STRING) {
						error("Invalid argument type for %s: %s != %s", key,
								dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_STRING));
						continue;
					}

					dbus_message_iter_get_basic(&value, &state);
					transport_set_state_from_string(t, state);

				}

			}

		}

	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/**
 * Register A2DP endpoints.
 *
 * @param conn D-Bus connection handler.
 * @param device HCI device name for which endpoints should be registered.
 * @param userdata Data passed to the endpoint handler.
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_register_a2dp(DBusConnection *conn, const char *device, void *userdata) {

	static a2dp_sbc_t a2dp_sbc = {
		.frequency =
			SBC_SAMPLING_FREQ_16000 |
			SBC_SAMPLING_FREQ_32000 |
			SBC_SAMPLING_FREQ_44100 |
			SBC_SAMPLING_FREQ_48000,
		.channel_mode =
			SBC_CHANNEL_MODE_MONO |
			SBC_CHANNEL_MODE_DUAL_CHANNEL |
			SBC_CHANNEL_MODE_STEREO |
			SBC_CHANNEL_MODE_JOINT_STEREO,
		.block_length =
			SBC_BLOCK_LENGTH_4 |
			SBC_BLOCK_LENGTH_8 |
			SBC_BLOCK_LENGTH_12 |
			SBC_BLOCK_LENGTH_16,
		.subbands =
			SBC_SUBBANDS_4 |
			SBC_SUBBANDS_8,
		.allocation_method =
			SBC_ALLOCATION_SNR |
			SBC_ALLOCATION_LOUDNESS,
		.min_bitpool = MIN_BITPOOL,
		.max_bitpool = MAX_BITPOOL,
	};

	static a2dp_mpeg_t a2dp_mpeg = {
		.layer =
			MPEG_LAYER_MP1 |
			MPEG_LAYER_MP2 |
			MPEG_LAYER_MP3,
		.crc = 1,
		.channel_mode =
			MPEG_CHANNEL_MODE_MONO |
			MPEG_CHANNEL_MODE_DUAL_CHANNEL |
			MPEG_CHANNEL_MODE_STEREO |
			MPEG_CHANNEL_MODE_JOINT_STEREO,
		.mpf = 1,
		.frequency =
			MPEG_SAMPLING_FREQ_16000 |
			MPEG_SAMPLING_FREQ_22050 |
			MPEG_SAMPLING_FREQ_24000 |
			MPEG_SAMPLING_FREQ_32000 |
			MPEG_SAMPLING_FREQ_44100 |
			MPEG_SAMPLING_FREQ_48000,
		.bitrate =
			MPEG_BIT_RATE_VBR |
			MPEG_BIT_RATE_320000 |
			MPEG_BIT_RATE_256000 |
			MPEG_BIT_RATE_224000 |
			MPEG_BIT_RATE_192000 |
			MPEG_BIT_RATE_160000 |
			MPEG_BIT_RATE_128000 |
			MPEG_BIT_RATE_112000 |
			MPEG_BIT_RATE_96000 |
			MPEG_BIT_RATE_80000 |
			MPEG_BIT_RATE_64000 |
			MPEG_BIT_RATE_56000 |
			MPEG_BIT_RATE_48000 |
			MPEG_BIT_RATE_40000 |
			MPEG_BIT_RATE_32000 |
			MPEG_BIT_RATE_FREE,
	};

	static a2dp_aac_t a2dp_aac = {
		.object_type =
			AAC_OBJECT_TYPE_MPEG2_AAC_LC |
			AAC_OBJECT_TYPE_MPEG4_AAC_LC |
			AAC_OBJECT_TYPE_MPEG4_AAC_LTP |
			AAC_OBJECT_TYPE_MPEG4_AAC_SCA,
		AAC_INIT_FREQUENCY(
			AAC_SAMPLING_FREQ_8000 |
			AAC_SAMPLING_FREQ_11025 |
			AAC_SAMPLING_FREQ_12000 |
			AAC_SAMPLING_FREQ_16000 |
			AAC_SAMPLING_FREQ_22050 |
			AAC_SAMPLING_FREQ_24000 |
			AAC_SAMPLING_FREQ_32000 |
			AAC_SAMPLING_FREQ_44100 |
			AAC_SAMPLING_FREQ_48000 |
			AAC_SAMPLING_FREQ_64000 |
			AAC_SAMPLING_FREQ_88200 |
			AAC_SAMPLING_FREQ_96000)
		.channels =
			AAC_CHANNELS_1 |
			AAC_CHANNELS_2,
		.vbr = 1,
		AAC_INIT_BITRATE(0xFFFF)
	};

	static struct endpoint {
		const char *uuid;
		const char *endpoint;
		uint8_t codec;
		const void *config;
		size_t config_size;
	} endpoints[] = {
		{
			BLUETOOTH_UUID_A2DP_SOURCE,
			BLUEZ_ENDPOINT_A2DP_SBC_SOURCE,
			A2DP_CODEC_SBC,
			&a2dp_sbc,
			sizeof(a2dp_sbc),
		},
		{
			BLUETOOTH_UUID_A2DP_SOURCE,
			BLUEZ_ENDPOINT_A2DP_MPEG12_SOURCE,
			A2DP_CODEC_MPEG12,
			&a2dp_mpeg,
			sizeof(a2dp_mpeg),
		},
		{
			BLUETOOTH_UUID_A2DP_SOURCE,
			BLUEZ_ENDPOINT_A2DP_MPEG24_SOURCE,
			A2DP_CODEC_MPEG24,
			&a2dp_aac,
			sizeof(a2dp_aac),
		},
		{
			BLUETOOTH_UUID_A2DP_SINK,
			BLUEZ_ENDPOINT_A2DP_SBC_SINK,
			A2DP_CODEC_SBC,
			&a2dp_sbc,
			sizeof(a2dp_sbc),
		},
		{
			BLUETOOTH_UUID_A2DP_SINK,
			BLUEZ_ENDPOINT_A2DP_MPEG12_SINK,
			A2DP_CODEC_MPEG12,
			&a2dp_mpeg,
			sizeof(a2dp_mpeg),
		},
		{
			BLUETOOTH_UUID_A2DP_SINK,
			BLUEZ_ENDPOINT_A2DP_MPEG24_SINK,
			A2DP_CODEC_MPEG24,
			&a2dp_aac,
			sizeof(a2dp_aac),
		},
	};

	char path[32];
	size_t i;

	snprintf(path, sizeof(path), "/org/bluez/%s", device);

	for (i = 0; i < sizeof(endpoints) / sizeof(struct endpoint); i++) {

		DBusMessage *msg, *rep;
		DBusMessageIter iter, iterarray;
		DBusError err;

		debug("Registering endpoint: %s: %s", endpoints[i].uuid, endpoints[i].endpoint);
		dbus_connection_register_object_path(conn, endpoints[i].endpoint, &endpoint_vtable, userdata);

		if ((msg = dbus_message_new_method_call("org.bluez", path,
						"org.bluez.Media1", "RegisterEndpoint")) == NULL) {
			error("Couldn't allocate D-Bus message");
			return -1;
		}

		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &endpoints[i].endpoint);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &iterarray);
		dbus_message_iter_append_dict_variant(&iterarray,
				"UUID", DBUS_TYPE_STRING, endpoints[i].uuid);
		dbus_message_iter_append_dict_variant(&iterarray,
				"Codec", DBUS_TYPE_BYTE, GINT_TO_POINTER(endpoints[i].codec));
		dbus_message_iter_append_dict_array(&iterarray,
				"Capabilities", DBUS_TYPE_BYTE, endpoints[i].config, endpoints[i].config_size);
		dbus_message_iter_close_container(&iter, &iterarray);

		dbus_error_init(&err);

		rep = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
		if (dbus_error_is_set(&err)) {
			warn("Couldn't register endpoint: %s", err.message);
			dbus_error_free(&err);
		}

		dbus_message_unref(msg);
		dbus_message_unref(rep);
	}

	return 0;
}

int bluez_register_hsp(DBusConnection *conn, void *userdata) {

	DBusMessage *msg, *rep;
	DBusMessageIter iter, iterarray;
	DBusError err;

	dbus_connection_register_object_path(conn, BLUEZ_PROFILE_HSP_AG, &profile_vtable, userdata);

	if ((msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
			"org.bluez.ProfileManager1", "RegisterProfile")) == NULL) {
		error("Couldn't allocate D-Bus message");
		return -1;
	}

	const char *path = BLUEZ_PROFILE_HSP_AG;
	const char *uuid = BLUETOOTH_UUID_HSP_AG;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &uuid);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &iterarray);
	dbus_message_iter_close_container(&iter, &iterarray);

	dbus_error_init(&err);

	rep = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
	dbus_message_unref(msg);

	if (dbus_error_is_set(&err)) {
		error("Cannot register profile: %s", err.message);
		dbus_error_free(&err);
	}

	if (!rep)
		return 0;

	dbus_message_unref(msg);
	dbus_message_unref(rep);
	return -1;
}

int bluez_register_signal_handler(DBusConnection *conn, const char *device, void *userdata) {
	(void)device;

	dbus_bus_add_match(conn,
			"type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='org.bluez.Adapter1'", NULL);
	dbus_bus_add_match(conn,
			"type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='org.bluez.Device1'", NULL);
	dbus_bus_add_match(conn,
			"type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='org.bluez.MediaTransport1'", NULL);

	if (!dbus_connection_add_filter(conn, bluez_signal_handler, userdata, NULL)) {
		error("Adding D-Bus filter callback failed");
		return -1;
	}

	return 0;
}
