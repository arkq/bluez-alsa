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
#include "log.h"
#include "transport.h"
#include "utils.h"


static struct ba_transport *bluez_transport_lookup(GHashTable *devices, const char *key) {

	GHashTableIter iter;
	struct ba_device *d;
	struct ba_transport *t;
	gpointer _key;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, &_key, (gpointer)&d)) {
		if ((t = g_hash_table_lookup(d->transports, key)) != NULL)
			return t;
	}

	return NULL;
}

static gboolean bluez_transport_remove(GHashTable *devices, const char *key) {

	GHashTableIter iter;
	struct ba_device *d;
	gpointer _key;

	g_hash_table_iter_init(&iter, devices);
	while (g_hash_table_iter_next(&iter, &_key, (gpointer)&d)) {
		if (g_hash_table_remove(d->transports, key)) {
			if (g_hash_table_size(d->transports) == 0)
				g_hash_table_iter_remove(&iter);
			return TRUE;
		}
	}

	return FALSE;
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

	if (cap->allocation_method & SBC_ALLOCATION_LOUDNESS)
		a2dp_sbc.allocation_method = SBC_ALLOCATION_LOUDNESS;
	else if (cap->allocation_method & SBC_ALLOCATION_SNR)
		a2dp_sbc.allocation_method = SBC_ALLOCATION_SNR;

	if (cap->subbands & SBC_SUBBANDS_8)
		a2dp_sbc.subbands = SBC_SUBBANDS_8;
	else if (cap->subbands & SBC_SUBBANDS_4)
		a2dp_sbc.subbands = SBC_SUBBANDS_4;
	else {
		error("No supported subbands: %u", cap->subbands);
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

	int bitpool = a2dp_default_bitpool(a2dp_sbc.frequency, a2dp_sbc.channel_mode);
	a2dp_sbc.min_bitpool = MAX(MIN_BITPOOL, cap->min_bitpool);
	a2dp_sbc.max_bitpool = MIN(bitpool, cap->max_bitpool);

	uint8_t *pconf = (uint8_t *)&a2dp_sbc;
	rep = dbus_message_new_method_return(msg);
	dbus_message_append_args(rep, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &pconf, size, DBUS_TYPE_INVALID);
	return rep;

fail:
	return dbus_message_new_error(msg, "org.bluez.Error.InvalidArguments", "Unable to select configuration");
}

static DBusMessage *bluez_endpoint_set_configuration(DBusConnection *conn, DBusMessage *msg, void *userdata) {

	DBusMessageIter arg_i, element_i;

	GHashTable *devices = (GHashTable *)userdata;
	enum ba_transport_type type = TRANSPORT_DISABLED;
	struct ba_transport *t;
	struct ba_device *d;

	const char *path;
	const char *dev_path = NULL, *uuid = NULL, *state = NULL;
	const uint8_t *config = NULL;
	uint16_t volume = 0;
	int size = 0;

	if (!dbus_message_iter_init(msg, &arg_i) || !dbus_message_has_signature(msg, "oa{sv}")) {
		error("Invalid signature for %s: %s != %s", "SetConfiguration()",
				dbus_message_get_signature(msg), "oa{sv}");
		goto fail;
	}

	dbus_message_iter_get_basic(&arg_i, &path);

	if (bluez_transport_lookup(devices, path) != NULL) {
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

			const char *endpoint_path = dbus_message_get_path(msg);
			dbus_message_iter_get_basic(&value, &uuid);

			if (strcmp(endpoint_path, ENDPOINT_A2DP_SOURCE) == 0) {
				if (strcmp(uuid, BLUETOOTH_UUID_A2DP_SOURCE) == 0)
					type = TRANSPORT_A2DP_SINK;
			} else if (strcmp(endpoint_path, ENDPOINT_A2DP_SINK) == 0) {
				if (strcmp(uuid, BLUETOOTH_UUID_A2DP_SINK) == 0)
					type = TRANSPORT_A2DP_SOURCE;
			}

			if (type == TRANSPORT_DISABLED) {
				error("UUID %s of transport %s incompatible with endpoint %s", uuid, path, endpoint_path);
				goto fail;
			}

		}
		else if (strcmp(key, "Codec") == 0) {
			/* TODO: Check if selected coded matches supported one. */
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
		else if (strcmp(key, "Volume") == 0) {

			if (var != DBUS_TYPE_UINT16) {
				error("Invalid argument type for %s: %s != %s", key,
						dbus_type_to_string(var), dbus_type_to_string(DBUS_TYPE_STRING));
				continue;
			}

			dbus_message_iter_get_basic(&value, &volume);

		}

	}

	if ((d = g_hash_table_lookup(devices, dev_path)) == NULL) {
		bdaddr_t addr;
		dbus_devpath_to_bdaddr(dev_path, &addr);
		/* TODO: Get real device name! */
		d = bluez_device_new(&addr, dev_path);
		g_hash_table_insert(devices, g_strdup(dev_path), d);
	}

	/* Create a new transport with a human-readable name. Since the transport
	 * name can not be obtained from the client, we will use a fall-back one. */
	if ((t = transport_new(type, transport_type_to_string(type))) == NULL) {
		error("Cannot create new transport: %s", strerror(errno));
		goto fail;
	}

	transport_set_dbus(t, conn, dbus_message_get_sender(msg), path);
	transport_set_codec(t, A2DP_CODEC_SBC, config, size);
	transport_set_state_from_string(t, state);
	t->volume = volume;

	g_hash_table_insert(d->transports, g_strdup(path), t);

	debug("Transport %s available for device %s", t->name, d->name);
	return dbus_message_new_method_return(msg);

fail:
	return dbus_message_new_error(msg, "org.bluez.Error.InvalidArguments", "Unable to set configuration");
}

static DBusMessage *bluez_endpoint_clear_configuration(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;

	GHashTable *devices = (GHashTable *)userdata;
	const char *path;
	DBusError err;

	dbus_error_init(&err);

	if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
		error("Endpoint ClearConfiguration(): %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	bluez_transport_remove(devices, path);
	return dbus_message_new_method_return(msg);

fail:
	return dbus_message_new_error(msg, "org.bluez.Error.InvalidArguments", "Unable to clear configuration");
}

static DBusMessage *bluez_endpoint_release(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)conn;
	(void)userdata;
	return dbus_message_new_error(msg, "org.bluez.Error.NotImplemented", "Method not implemented");
}

static DBusHandlerResult bluez_endpoint_handler(DBusConnection *conn, DBusMessage *msg, void *userdata) {

	const char *path = dbus_message_get_path(msg);
	DBusMessage *rep = NULL;

	debug("Endpoint handler: %s", path);

	if (strcmp(path, ENDPOINT_A2DP_SOURCE) != 0 && strcmp(path, ENDPOINT_A2DP_SINK) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_is_method_call(msg, "org.bluez.MediaEndpoint1", "SelectConfiguration"))
		rep = bluez_endpoint_select_configuration(conn, msg, userdata);
	else if (dbus_message_is_method_call(msg, "org.bluez.MediaEndpoint1", "SetConfiguration"))
		rep = bluez_endpoint_set_configuration(conn, msg, userdata);
	else if (dbus_message_is_method_call(msg, "org.bluez.MediaEndpoint1", "ClearConfiguration"))
		rep = bluez_endpoint_clear_configuration(conn, msg, userdata);
	else if (dbus_message_is_method_call(msg, "org.bluez.MediaEndpoint1", "Release"))
		rep = bluez_endpoint_release(conn, msg, userdata);
	else
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (rep != NULL) {
		dbus_connection_send(conn, rep, NULL);
		dbus_message_unref(rep);
	}

  return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult bluez_profile_handler(DBusConnection *conn, DBusMessage *msg, void *userdata) {
	(void)userdata;

	const char *path = dbus_message_get_path(msg);
	DBusMessage *rep = NULL;

	debug("Profile handler: %s", path);

	if (strcmp(path, PROFILE_HSP_AG) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_is_method_call(msg, "org.bluez.ProfileManager1", "Release")) {
	}
	else if (dbus_message_is_method_call(msg, "org.bluez.ProfileManager1", "RequestDisconnection")) {
	}
	else if (dbus_message_is_method_call(msg, "org.bluez.ProfileManager1", "NewConnection")) {
	}
	else
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (rep != NULL) {
		dbus_connection_send(conn, rep, NULL);
		dbus_message_unref(rep);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

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

			if ((t = bluez_transport_lookup(devices, path)) == NULL) {
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

static void dbus_add_dict_variant_entry(DBusMessageIter *iter, char *key, int type, const void *value) {
	DBusMessageIter dict, variant;
	dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, (char *)&type, &variant);
	dbus_message_iter_append_basic(&variant, type, &value);
	dbus_message_iter_close_container(&dict, &variant);
	dbus_message_iter_close_container(iter, &dict);
}

static void dbus_add_dict_array_entry(DBusMessageIter *iter, char *key, int type, void *buf, int elements) {
	DBusMessageIter dict, variant, array;
	char array_type[5] = "a";
	strncat(array_type, (char *)&type, sizeof(array_type));
	dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, array_type, &variant);
	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, (char *)&type, &array);
	dbus_message_iter_append_fixed_array(&array, type, &buf, elements);
	dbus_message_iter_close_container(&variant, &array);
	dbus_message_iter_close_container(&dict, &variant);
	dbus_message_iter_close_container(iter, &dict);
}

struct ba_device *bluez_device_new(bdaddr_t *addr, const char *name) {

	struct ba_device *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return NULL;

	bacpy(&d->addr, addr);
	d->name = strdup(name);
	d->transports = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)transport_free);

	return d;
}

void bluez_device_free(struct ba_device *d) {
	if (d == NULL)
		return;
	free(d->name);
	free(d);
}

int bluez_register_endpoint(DBusConnection *conn, const char *device, const char *endpoint, const char *uuid) {

	DBusMessage *msg, *rep;
	DBusMessageIter iter, iterarray;
	DBusError err;
	char path[32] = "/org/bluez";

	a2dp_sbc_t a2dp_sbc = {
		.channel_mode =
			SBC_CHANNEL_MODE_MONO |
			SBC_CHANNEL_MODE_DUAL_CHANNEL |
			SBC_CHANNEL_MODE_STEREO |
			SBC_CHANNEL_MODE_JOINT_STEREO,
		.frequency =
			SBC_SAMPLING_FREQ_16000 |
			SBC_SAMPLING_FREQ_32000 |
			SBC_SAMPLING_FREQ_44100 |
			SBC_SAMPLING_FREQ_48000,
		.allocation_method =
			SBC_ALLOCATION_SNR |
			SBC_ALLOCATION_LOUDNESS,
		.subbands =
			SBC_SUBBANDS_4 |
			SBC_SUBBANDS_8,
		.block_length =
			SBC_BLOCK_LENGTH_4 |
			SBC_BLOCK_LENGTH_8 |
			SBC_BLOCK_LENGTH_12 |
			SBC_BLOCK_LENGTH_16,
		.min_bitpool = MIN_BITPOOL,
		.max_bitpool = MAX_BITPOOL,
	};

	if (device != NULL)
		strcat(strcat(path, "/"), device);

	msg = dbus_message_new_method_call("org.bluez", path, "org.bluez.Media1", "RegisterEndpoint");

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &endpoint);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &iterarray);
	dbus_add_dict_variant_entry(&iterarray, "UUID", DBUS_TYPE_STRING, uuid);
	dbus_add_dict_variant_entry(&iterarray, "Codec", DBUS_TYPE_BYTE, A2DP_CODEC_SBC);
	dbus_add_dict_array_entry(&iterarray, "Capabilities", DBUS_TYPE_BYTE, &a2dp_sbc, sizeof(a2dp_sbc));
	dbus_message_iter_close_container (&iter, &iterarray);

	dbus_error_init(&err);

	rep = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
	dbus_message_unref(msg);

	if (dbus_error_is_set(&err))
		error("Cannot register endpoint: %s", err.message);
	dbus_error_free(&err);

	if (!rep)
		return 0;

	dbus_message_unref(rep);
	return -1;
}

int bluez_register_profile(DBusConnection *conn, const char *profile, const char *uuid) {

	DBusMessage *msg, *rep;
	DBusMessageIter iter, iterarray;
	DBusError err;

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile");

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &profile);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &uuid);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &iterarray);
	dbus_message_iter_close_container(&iter, &iterarray);

	dbus_error_init(&err);

	rep = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
	dbus_message_unref(msg);

	if (dbus_error_is_set(&err))
		fprintf(stderr, "DBus error: %s: %s\n", __FUNCTION__, err.message);
	dbus_error_free(&err);

	if (!rep)
		return 0;

	dbus_message_unref(rep);
	return 1;
}

static const DBusObjectPathVTable endpoint_vtable = {
	.message_function = bluez_endpoint_handler,
};

static const DBusObjectPathVTable profile_vtable = {
	.message_function = bluez_profile_handler,
};

int bluez_register_a2dp_source(DBusConnection *conn, const char *device, void *userdata) {
	dbus_connection_register_object_path(conn, ENDPOINT_A2DP_SOURCE, &endpoint_vtable, userdata);
	return bluez_register_endpoint(conn, device, ENDPOINT_A2DP_SOURCE, BLUETOOTH_UUID_A2DP_SOURCE);
}

int bluez_register_a2dp_sink(DBusConnection *conn, const char *device, void *userdata) {
	dbus_connection_register_object_path(conn, ENDPOINT_A2DP_SINK, &endpoint_vtable, userdata);
	return bluez_register_endpoint(conn, device, ENDPOINT_A2DP_SINK, BLUETOOTH_UUID_A2DP_SINK);
}

int bluez_register_profile_hsp_ag(DBusConnection *conn, void *userdata) {
	dbus_connection_register_object_path(conn, PROFILE_HSP_AG, &profile_vtable, userdata);
	return bluez_register_profile(conn, PROFILE_HSP_AG, BLUETOOTH_UUID_HSP_AG);
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
