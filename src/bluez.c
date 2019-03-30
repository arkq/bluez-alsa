/*
 * BlueALSA - bluez.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluez.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib.h>

#include "a2dp-codecs.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa.h"
#include "bluez-a2dp.h"
#include "bluez-iface.h"
#include "ctl.h"
#include "utils.h"
#include "shared/ctl-proto.h"
#include "shared/log.h"

/* Compatibility patch for glib < 2.42. */
#ifndef G_DBUS_ERROR_UNKNOWN_OBJECT
# define G_DBUS_ERROR_UNKNOWN_OBJECT G_DBUS_ERROR_FAILED
#endif

/**
 * Structure describing registered D-Bus object. */
struct dbus_object_data {
	/* D-Bus object registration ID */
	unsigned int id;
	/* associated adapter */
	const struct ba_adapter *adapter;
	struct ba_transport_type ttype;
	/* determine whether profile is used */
	bool connected;
};

static GHashTable *dbus_object_data_map = NULL;

/**
 * Check whether D-Bus adapter matches our configuration. */
static bool bluez_match_dbus_adapter(
		const char *adapter_path,
		const char *adapter_address) {

	/* if configuration is empty, match everything */
	if (config.hci_filter->len == 0)
		return true;

	/* get the last component of the path */
	if ((adapter_path = strrchr(adapter_path, '/')) != NULL)
		adapter_path++;

	size_t i;
	for (i = 0; i < config.hci_filter->len; i++)
		if (strcasecmp(adapter_path, g_array_index(config.hci_filter, char *, i)) == 0 ||
				strcasecmp(adapter_address, g_array_index(config.hci_filter, char *, i)) == 0)
			return true;

	return false;
}

/**
 * Get D-Bus object reference count for given transport type. */
static int bluez_get_dbus_object_count(
		const struct ba_adapter *adapter,
		struct ba_transport_type ttype) {

	GHashTableIter iter;
	struct dbus_object_data *obj;
	int count = 0;

	g_hash_table_iter_init(&iter, dbus_object_data_map);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&obj))
		if (obj->adapter == adapter &&
				obj->ttype.profile == ttype.profile &&
				obj->ttype.codec == ttype.codec &&
				obj->connected)
			count++;

	return count;
}

/**
 * Check whether channel mode configuration is valid. */
static bool bluez_a2dp_codec_check_channel_mode(
		const struct bluez_a2dp_codec *codec,
		unsigned int capabilities) {

	size_t i;

	for (i = 0; i < codec->channels_size; i++)
		if (capabilities == codec->channels[i].value)
			return true;

	return false;
}

/**
 * Check whether sampling frequency configuration is valid. */
static bool bluez_a2dp_codec_check_sampling_freq(
		const struct bluez_a2dp_codec *codec,
		unsigned int capabilities) {

	size_t i;

	for (i = 0; i < codec->samplings_size; i++)
		if (capabilities == codec->samplings[i].value)
			return true;

	return false;
}

/**
 * Select (best) channel mode configuration. */
static unsigned int bluez_a2dp_codec_select_channel_mode(
		const struct bluez_a2dp_codec *codec,
		unsigned int capabilities) {

	size_t i;

	/* If monophonic sound has been forced, check whether given codec supports
	 * such a channel mode. Since mono channel mode shall be stored at index 0
	 * we can simply check for its existence with a simple index lookup. */
	if (config.a2dp.force_mono &&
			codec->channels[0].mode == BLUEZ_A2DP_CHM_MONO &&
			capabilities & codec->channels[0].value)
		return codec->channels[0].value;

	/* favor higher number of channels */
	for (i = codec->channels_size; i > 0; i--)
		if (capabilities & codec->channels[i - 1].value)
			return codec->channels[i - 1].value;

	return 0;
}

/**
 * Select (best) sampling frequency configuration. */
static unsigned int bluez_a2dp_codec_select_sampling_freq(
		const struct bluez_a2dp_codec *codec,
		unsigned int capabilities) {

	size_t i;

	if (config.a2dp.force_44100)
		for (i = 0; i < codec->samplings_size; i++)
			if (codec->samplings[i].frequency == 44100) {
				if (capabilities & codec->samplings[i].value)
					return codec->samplings[i].value;
				break;
			}

	/* favor higher sampling frequencies */
	for (i = codec->samplings_size; i > 0; i--)
		if (capabilities & codec->samplings[i - 1].value)
			return codec->samplings[i - 1].value;

	return 0;
}

/**
 * Set transport state using BlueZ state string. */
static int bluez_a2dp_set_transport_state(
		struct ba_transport *t,
		const char *state) {

	if (strcmp(state, BLUEZ_TRANSPORT_STATE_IDLE) == 0)
		return ba_transport_set_state(t, TRANSPORT_IDLE);
	else if (strcmp(state, BLUEZ_TRANSPORT_STATE_PENDING) == 0)
		return ba_transport_set_state(t, TRANSPORT_PENDING);
	else if (strcmp(state, BLUEZ_TRANSPORT_STATE_ACTIVE) == 0)
		return ba_transport_set_state(t, TRANSPORT_ACTIVE);

	warn("Invalid state: %s", state);
	return -1;
}

static void bluez_endpoint_select_configuration(GDBusMethodInvocation *inv, void *userdata) {

	const char *endpoint_path = g_dbus_method_invocation_get_object_path(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	const struct bluez_a2dp_codec *codec = userdata;

	const uint8_t *data;
	uint8_t *capabilities;
	size_t size = 0;

	params = g_variant_get_child_value(params, 0);
	data = g_variant_get_fixed_array(params, &size, sizeof(uint8_t));
	capabilities = g_memdup(data, size);
	g_variant_unref(params);

	if (size != codec->cfg_size) {
		error("Invalid capabilities size: %zu != %zu", size, codec->cfg_size);
		goto fail;
	}

	switch (codec->id) {
	case A2DP_CODEC_SBC: {

		a2dp_sbc_t *cap = (a2dp_sbc_t *)capabilities;
		unsigned int cap_chm = cap->channel_mode;
		unsigned int cap_freq = cap->frequency;

		if ((cap->channel_mode = bluez_a2dp_codec_select_channel_mode(codec, cap_chm)) == 0) {
			error("No supported channel modes: %#x", cap_chm);
			goto fail;
		}

		if ((cap->frequency = bluez_a2dp_codec_select_sampling_freq(codec, cap_freq)) == 0) {
			error("No supported sampling frequencies: %#x", cap_freq);
			goto fail;
		}

		if (cap->block_length & SBC_BLOCK_LENGTH_16)
			cap->block_length = SBC_BLOCK_LENGTH_16;
		else if (cap->block_length & SBC_BLOCK_LENGTH_12)
			cap->block_length = SBC_BLOCK_LENGTH_12;
		else if (cap->block_length & SBC_BLOCK_LENGTH_8)
			cap->block_length = SBC_BLOCK_LENGTH_8;
		else if (cap->block_length & SBC_BLOCK_LENGTH_4)
			cap->block_length = SBC_BLOCK_LENGTH_4;
		else {
			error("No supported block lengths: %#x", cap->block_length);
			goto fail;
		}

		if (cap->subbands & SBC_SUBBANDS_8)
			cap->subbands = SBC_SUBBANDS_8;
		else if (cap->subbands & SBC_SUBBANDS_4)
			cap->subbands = SBC_SUBBANDS_4;
		else {
			error("No supported subbands: %#x", cap->subbands);
			goto fail;
		}

		if (cap->allocation_method & SBC_ALLOCATION_LOUDNESS)
			cap->allocation_method = SBC_ALLOCATION_LOUDNESS;
		else if (cap->allocation_method & SBC_ALLOCATION_SNR)
			cap->allocation_method = SBC_ALLOCATION_SNR;
		else {
			error("No supported allocation: %#x", cap->allocation_method);
			goto fail;
		}

		int bitpool = a2dp_sbc_default_bitpool(cap->frequency, cap->channel_mode);
		cap->min_bitpool = MAX(SBC_MIN_BITPOOL, cap->min_bitpool);
		cap->max_bitpool = MIN(bitpool, cap->max_bitpool);

		break;
	}

#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12: {

		a2dp_mpeg_t *cap = (a2dp_mpeg_t *)capabilities;
		unsigned int cap_chm = cap->channel_mode;
		unsigned int cap_freq = cap->frequency;

		if ((cap->channel_mode = bluez_a2dp_codec_select_channel_mode(codec, cap_chm)) == 0) {
			error("No supported channel modes: %#x", cap_chm);
			goto fail;
		}

		if ((cap->frequency = bluez_a2dp_codec_select_sampling_freq(codec, cap_freq)) == 0) {
			error("No supported sampling frequencies: %#x", cap_freq);
			goto fail;
		}

		break;
	}
#endif

#if ENABLE_AAC
	case A2DP_CODEC_MPEG24: {

		a2dp_aac_t *cap = (a2dp_aac_t *)capabilities;
		unsigned int cap_chm = cap->channels;
		unsigned int cap_freq = AAC_GET_FREQUENCY(*cap);

		if (cap->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_SCA)
			cap->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_SCA;
		else if (cap->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LTP)
			cap->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LTP;
		else if (cap->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC)
			cap->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC;
		else if (cap->object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC)
			cap->object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC;
		else {
			error("No supported object type: %#x", cap->object_type);
			goto fail;
		}

		if ((cap->channels = bluez_a2dp_codec_select_channel_mode(codec, cap_chm)) == 0) {
			error("No supported channels: %#x", cap_chm);
			goto fail;
		}

		unsigned int freq;
		if ((freq = bluez_a2dp_codec_select_sampling_freq(codec, cap_freq)) != 0)
			AAC_SET_FREQUENCY(*cap, freq);
		else {
			error("No supported sampling frequencies: %#x", cap_freq);
			goto fail;
		}

		break;
	}
#endif

#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX: {

		a2dp_aptx_t *cap = (a2dp_aptx_t *)capabilities;
		unsigned int cap_chm = cap->channel_mode;
		unsigned int cap_freq = cap->frequency;

		if ((cap->channel_mode = bluez_a2dp_codec_select_channel_mode(codec, cap_chm)) == 0) {
			error("No supported channel modes: %#x", cap_chm);
			goto fail;
		}

		if ((cap->frequency = bluez_a2dp_codec_select_sampling_freq(codec, cap_freq)) == 0) {
			error("No supported sampling frequencies: %#x", cap_freq);
			goto fail;
		}

		break;
	}
#endif

#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC: {

		a2dp_ldac_t *cap = (a2dp_ldac_t *)capabilities;
		unsigned int cap_chm = cap->channel_mode;
		unsigned int cap_freq = cap->frequency;

		if ((cap->channel_mode = bluez_a2dp_codec_select_channel_mode(codec, cap_chm)) == 0) {
			error("No supported channel modes: %#x", cap_chm);
			goto fail;
		}

		if ((cap->frequency = bluez_a2dp_codec_select_sampling_freq(codec, cap_freq)) == 0) {
			error("No supported sampling frequencies: %#x", cap_freq);
			goto fail;
		}

		break;
	}
#endif

	default:
		debug("Endpoint path not supported: %s", endpoint_path);
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_UNKNOWN_OBJECT, "Not supported");
		goto final;
	}

	GVariantBuilder caps;
	size_t i;

	g_variant_builder_init(&caps, G_VARIANT_TYPE("ay"));
	for (i = 0; i < size; i++)
		g_variant_builder_add(&caps, "y", capabilities[i]);

	g_dbus_method_invocation_return_value(inv, g_variant_new("(ay)", &caps));
	g_variant_builder_clear(&caps);

	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Invalid capabilities");

final:
	g_free(capabilities);
}

static int bluez_endpoint_set_configuration(GDBusMethodInvocation *inv, void *userdata) {

	const gchar *sender = g_dbus_method_invocation_get_sender(inv);
	const gchar *endpoint_path = g_dbus_method_invocation_get_object_path(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	const struct bluez_a2dp_codec *codec = userdata;
	const uint16_t codec_id = codec->id;

	struct ba_adapter *a = NULL;
	struct ba_transport *t = NULL;
	struct ba_device *d = NULL;

	char *state = NULL;
	char *device_path = NULL;
	uint8_t *configuration = NULL;
	uint16_t volume = 127;
	uint16_t delay = 150;
	size_t size = 0;
	int ret = 0;

	const char *transport_path;
	GVariantIter *properties;
	GVariant *value = NULL;
	const char *property;

	g_variant_get(params, "(&oa{sv})", &transport_path, &properties);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {

		if (strcmp(property, "Device") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) {
				error("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "o");
				goto fail;
			}

			device_path = g_variant_dup_string(value, NULL);

		}
		else if (strcmp(property, "UUID") == 0) {
		}
		else if (strcmp(property, "Codec") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE)) {
				error("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "y");
				goto fail;
			}

			if ((codec_id & 0xFF) != g_variant_get_byte(value)) {
				error("Invalid configuration: %s", "Codec mismatch");
				goto fail;
			}

		}
		else if (strcmp(property, "Configuration") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING)) {
				error("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "ay");
				goto fail;
			}

			const guchar *capabilities = g_variant_get_fixed_array(value, &size, sizeof(uint8_t));
			unsigned int cap_chm = 0;
			unsigned int cap_freq = 0;

			configuration = g_memdup(capabilities, size);

			if (size != codec->cfg_size) {
				error("Invalid configuration: %s", "Invalid size");
				goto fail;
			}

			switch (codec_id) {
			case A2DP_CODEC_SBC: {

				const a2dp_sbc_t *cap = (a2dp_sbc_t *)capabilities;
				cap_chm = cap->channel_mode;
				cap_freq = cap->frequency;

				if (cap->allocation_method != SBC_ALLOCATION_SNR &&
						cap->allocation_method != SBC_ALLOCATION_LOUDNESS) {
					error("Invalid configuration: %s", "Invalid allocation method");
					goto fail;
				}

				if (cap->subbands != SBC_SUBBANDS_4 &&
						cap->subbands != SBC_SUBBANDS_8) {
					error("Invalid configuration: %s", "Invalid SBC subbands");
					goto fail;
				}

				if (cap->block_length != SBC_BLOCK_LENGTH_4 &&
						cap->block_length != SBC_BLOCK_LENGTH_8 &&
						cap->block_length != SBC_BLOCK_LENGTH_12 &&
						cap->block_length != SBC_BLOCK_LENGTH_16) {
					error("Invalid configuration: %s", "Invalid block length");
					goto fail;
				}

				break;
			}

#if ENABLE_MPEG
			case A2DP_CODEC_MPEG12: {
				a2dp_mpeg_t *cap = (a2dp_mpeg_t *)capabilities;
				cap_chm = cap->channel_mode;
				cap_freq = cap->frequency;
				break;
			}
#endif

#if ENABLE_AAC
			case A2DP_CODEC_MPEG24: {

				const a2dp_aac_t *cap = (a2dp_aac_t *)capabilities;
				cap_chm = cap->channels;
				cap_freq = AAC_GET_FREQUENCY(*cap);

				if (cap->object_type != AAC_OBJECT_TYPE_MPEG2_AAC_LC &&
						cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_LC &&
						cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_LTP &&
						cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_SCA) {
					error("Invalid configuration: %s", "Invalid object type");
					goto fail;
				}

				break;
			}
#endif

#if ENABLE_APTX
			case A2DP_CODEC_VENDOR_APTX: {
				a2dp_aptx_t *cap = (a2dp_aptx_t *)capabilities;
				cap_chm = cap->channel_mode;
				cap_freq = cap->frequency;
				break;
			}
#endif

#if ENABLE_LDAC
			case A2DP_CODEC_VENDOR_LDAC: {
				a2dp_ldac_t *cap = (a2dp_ldac_t *)capabilities;
				cap_chm = cap->channel_mode;
				cap_freq = cap->frequency;
				break;
			}
#endif

			default:
				error("Invalid configuration: %s", "Unsupported codec");
				goto fail;
			}

			if (!bluez_a2dp_codec_check_channel_mode(codec, cap_chm)) {
				error("Invalid configuration: %s", "Invalid channel mode");
				goto fail;
			}

			if (!bluez_a2dp_codec_check_sampling_freq(codec, cap_freq)) {
				error("Invalid configuration: %s", "Invalid sampling frequency");
				goto fail;
			}

		}
		else if (strcmp(property, "State") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
				error("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "s");
				goto fail;
			}

			state = g_variant_dup_string(value, NULL);

		}
		else if (strcmp(property, "Delay") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				error("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "q");
				goto fail;
			}

			delay = g_variant_get_uint16(value);

		}
		else if (strcmp(property, "Volume") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				error("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "q");
				goto fail;
			}

			/* received volume is in range [0, 127]*/
			volume = g_variant_get_uint16(value);

		}

		g_variant_unref(value);
		value = NULL;
	}

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(transport_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL &&
			(a = ba_adapter_new(hci_dev_id, NULL)) == NULL) {
		error("Couldn't create new adapter: %s", strerror(errno));
		goto fail;
	}

	/* we are going to modify the devices hash-map */
	pthread_mutex_lock(&a->devices_mutex);

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL &&
			(d = ba_device_new(a, &addr, NULL)) == NULL) {
		error("Couldn't create new device: %s", device_path);
		goto fail;
	}

	if (ba_transport_lookup(d, transport_path) != NULL) {
		error("Transport already configured: %s", transport_path);
		goto fail;
	}

	if ((t = ba_transport_new_a2dp(d, g_dbus_bluez_object_path_to_transport_type(endpoint_path),
					sender, transport_path, configuration, size)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->a2dp.ch1_volume = volume;
	t->a2dp.ch2_volume = volume;
	t->a2dp.delay = delay;

	debug("%s configured for device %s",
			ba_transport_type_to_string(t->type),
			batostr_(&d->addr));
	debug("Configuration: channels: %u, sampling: %u",
			ba_transport_get_channels(t), ba_transport_get_sampling(t));

	bluez_a2dp_set_transport_state(t, state);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to set configuration");
	ret = -1;

final:
	if (a != NULL)
		pthread_mutex_unlock(&a->devices_mutex);
	g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
	g_free(device_path);
	g_free(configuration);
	g_free(state);
	return ret;
}

static void bluez_endpoint_clear_configuration(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *transport_path;
	g_variant_get(params, "(&o)", &transport_path);

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(transport_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL)
		goto fail;

	pthread_mutex_lock(&a->devices_mutex);

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(transport_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) != NULL &&
			(t = ba_transport_lookup(d, transport_path)) != NULL)
		ba_transport_free(t);

	pthread_mutex_unlock(&a->devices_mutex);

fail:
	g_object_unref(inv);
}

static void bluez_endpoint_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusConnection *conn = g_dbus_method_invocation_get_connection(inv);
	const char *endpoint_path = g_dbus_method_invocation_get_object_path(inv);
	gpointer hash = GINT_TO_POINTER(g_str_hash(endpoint_path));
	struct dbus_object_data *obj;

	debug("Releasing endpoint: %s", endpoint_path);

	if ((obj = g_hash_table_lookup(dbus_object_data_map, hash)) != NULL) {
		g_dbus_connection_unregister_object(conn, obj->id);
		g_hash_table_remove(dbus_object_data_map, hash);
	}

	g_object_unref(inv);
}

void bluez_register_a2dp(struct ba_adapter *adapter);

static void bluez_endpoint_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface;
	(void)params;

	debug("Endpoint method call: %s.%s()", interface, method);

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(path);
	gpointer hash = GINT_TO_POINTER(g_str_hash(path));
	struct dbus_object_data *obj;
	struct ba_adapter *a;

	if (strcmp(method, "SelectConfiguration") == 0)
		bluez_endpoint_select_configuration(invocation, userdata);
	else if (strcmp(method, "SetConfiguration") == 0) {
		if (bluez_endpoint_set_configuration(invocation, userdata) == 0) {
			obj = g_hash_table_lookup(dbus_object_data_map, hash);
			obj->connected = true;
			if ((a = ba_adapter_lookup(hci_dev_id)) != NULL)
				bluez_register_a2dp(a);
		}
	}
	else if (strcmp(method, "ClearConfiguration") == 0) {
		bluez_endpoint_clear_configuration(invocation, userdata);
		obj = g_hash_table_lookup(dbus_object_data_map, hash);
		obj->connected = false;
	}
	else if (strcmp(method, "Release") == 0)
		bluez_endpoint_release(invocation, userdata);
	else
		warn("Unsupported endpoint method: %s", method);

}

static void endpoint_free(gpointer data) {
	(void)data;
}

/**
 * Register A2DP endpoint.
 *
 * @param adapter
 * @param uuid
 * @param profile
 * @param codec
 * @return On success this function returns 0. Otherwise -1 is returned. */
static int bluez_register_a2dp_endpoint(
		const struct ba_adapter *adapter,
		const char *uuid,
		uint32_t profile,
		const struct bluez_a2dp_codec *codec) {

	static GDBusInterfaceVTable vtable = {
		.method_call = bluez_endpoint_method_call,
	};
	struct ba_transport_type ttype = {
		.profile = profile,
		.codec = codec->id,
	};
	struct dbus_object_data dbus_object = {
		.adapter = adapter,
		.ttype = ttype,
	};

	char endpoint_path[64];
	snprintf(endpoint_path, sizeof(endpoint_path), "/org/bluez/%s%s/%d",
			adapter->hci_name,
			g_dbus_transport_type_to_bluez_object_path(ttype),
			bluez_get_dbus_object_count(adapter, ttype) + 1);

	gpointer hash = GINT_TO_POINTER(g_str_hash(endpoint_path));
	if (g_hash_table_lookup(dbus_object_data_map, hash) != NULL) {
		debug("Endpoint already registered: %s", endpoint_path);
		return 0;
	}

	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;
	size_t i;

	debug("Registering endpoint: %s", endpoint_path);
	if ((dbus_object.id = g_dbus_connection_register_object(conn, endpoint_path,
					(GDBusInterfaceInfo *)&bluez_iface_endpoint, &vtable,
					(void *)codec, endpoint_free, &err)) == 0)
		goto fail;

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adapter->bluez_dbus_path,
			BLUEZ_IFACE_MEDIA, "RegisterEndpoint");

	GVariantBuilder caps;
	GVariantBuilder properties;

	g_variant_builder_init(&caps, G_VARIANT_TYPE("ay"));
	g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));

	for (i = 0; i < codec->cfg_size; i++)
		g_variant_builder_add(&caps, "y", ((uint8_t *)codec->cfg)[i]);

	g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(uuid));
	g_variant_builder_add(&properties, "{sv}", "DelayReporting", g_variant_new_boolean(TRUE));
	g_variant_builder_add(&properties, "{sv}", "Codec", g_variant_new_byte(codec->id));
	g_variant_builder_add(&properties, "{sv}", "Capabilities", g_variant_builder_end(&caps));

	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", endpoint_path, &properties));
	g_variant_builder_clear(&properties);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	g_hash_table_insert(dbus_object_data_map, hash,
			g_memdup(&dbus_object, sizeof(dbus_object)));

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't register endpoint: %s", err->message);
		g_dbus_connection_unregister_object(conn, dbus_object.id);
		g_error_free(err);
	}

	return ret;
}

/**
 * Register A2DP endpoints. */
void bluez_register_a2dp(struct ba_adapter *adapter) {

	const struct bluez_a2dp_codec **cc = config.a2dp.codecs;

	while (*cc != NULL) {
		const struct bluez_a2dp_codec *c = *cc++;
		switch (c->dir) {
		case BLUEZ_A2DP_SOURCE:
			if (config.enable.a2dp_source)
				bluez_register_a2dp_endpoint(adapter, BLUETOOTH_UUID_A2DP_SOURCE, BA_TRANSPORT_PROFILE_A2DP_SOURCE, c);
			break;
		case BLUEZ_A2DP_SINK:
			if (config.enable.a2dp_sink)
				bluez_register_a2dp_endpoint(adapter, BLUETOOTH_UUID_A2DP_SINK, BA_TRANSPORT_PROFILE_A2DP_SINK, c);
			break;
		}
	}

}

static void bluez_profile_new_connection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	const gchar *sender = g_dbus_method_invocation_get_sender(inv);
	const gchar *profile_path = g_dbus_method_invocation_get_object_path(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *device_path;
	GVariantIter *properties;
	GUnixFDList *fd_list;
	GError *err = NULL;
	int fd = -1;

	g_variant_get(params, "(&oha{sv})", &device_path, &fd, &properties);

	fd_list = g_dbus_message_get_unix_fd_list(msg);
	if ((fd = g_unix_fd_list_get(fd_list, 0, &err)) == -1) {
		error("Couldn't obtain RFCOMM socket: %s", err->message);
		goto fail;
	}

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(device_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL &&
			(a = ba_adapter_new(hci_dev_id, NULL)) == NULL) {
		error("Couldn't create new adapter: %s", strerror(errno));
		goto fail;
	}

	/* we are going to modify the devices hash-map */
	pthread_mutex_lock(&a->devices_mutex);

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL &&
			(d = ba_device_new(a, &addr, NULL)) == NULL) {
		error("Couldn't create new device: %s", strerror(errno));
		goto fail;
	}

	if ((t = ba_transport_new_rfcomm(d, g_dbus_bluez_object_path_to_transport_type(profile_path),
					sender, device_path)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->bt_fd = fd;

	debug("%s configured for device %s",
			ba_transport_type_to_string(t->type),
			batostr_(&d->addr));

	ba_transport_set_state(t, TRANSPORT_ACTIVE);
	ba_transport_set_state(t->rfcomm.sco, TRANSPORT_ACTIVE);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to connect profile");
	if (fd != -1)
		close(fd);

final:
	if (a != NULL)
		pthread_mutex_unlock(&a->devices_mutex);
	g_variant_iter_free(properties);
	if (err != NULL)
		g_error_free(err);
}

static void bluez_profile_request_disconnection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *device_path;
	g_variant_get(params, "(&o)", &device_path);

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(device_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL)
		goto fail;

	pthread_mutex_lock(&a->devices_mutex);

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) != NULL &&
			(t = ba_transport_lookup(d, device_path)) != NULL)
		ba_transport_free(t);

	pthread_mutex_unlock(&a->devices_mutex);

fail:
	g_object_unref(inv);
}

static void bluez_profile_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusConnection *conn = g_dbus_method_invocation_get_connection(inv);
	const char *profile_path = g_dbus_method_invocation_get_object_path(inv);
	gpointer hash = GINT_TO_POINTER(g_str_hash(profile_path));
	struct dbus_object_data *obj;

	debug("Releasing profile: %s", profile_path);

	if ((obj = g_hash_table_lookup(dbus_object_data_map, hash)) != NULL) {
		g_dbus_connection_unregister_object(conn, obj->id);
		g_hash_table_remove(dbus_object_data_map, hash);
	}

	g_object_unref(inv);
}

static void bluez_profile_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)params;

	debug("Profile method call: %s.%s()", interface, method);

	if (strcmp(method, "NewConnection") == 0)
		bluez_profile_new_connection(invocation, userdata);
	else if (strcmp(method, "RequestDisconnection") == 0)
		bluez_profile_request_disconnection(invocation, userdata);
	else if (strcmp(method, "Release") == 0)
		bluez_profile_release(invocation, userdata);
	else
		warn("Unsupported profile method: %s", method);

}

/**
 * Register Bluetooth Audio Profile.
 *
 * @param uuid
 * @param profile
 * @param version
 * @param features
 * @return On success this function returns 0. Otherwise -1 is returned. */
static int bluez_register_profile(
		const char *uuid,
		uint32_t profile,
		uint16_t version,
		uint16_t features) {

	static GDBusInterfaceVTable vtable = {
		.method_call = bluez_profile_method_call,
	};
	struct ba_transport_type ttype = {
		.profile = profile,
	};
	struct dbus_object_data dbus_object = {
		.ttype = ttype,
	};

	char profile_path[64];
	snprintf(profile_path, sizeof(profile_path), "/org/bluez%s",
			g_dbus_transport_type_to_bluez_object_path(ttype));

	gpointer hash = GINT_TO_POINTER(g_str_hash(profile_path));
	if (g_hash_table_lookup(dbus_object_data_map, hash) != NULL) {
		debug("Profile already registered: %s", profile_path);
		return 0;
	}

	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	debug("Registering profile: %s", profile_path);
	if ((dbus_object.id = g_dbus_connection_register_object(conn, profile_path,
					(GDBusInterfaceInfo *)&bluez_iface_profile, &vtable,
					NULL, NULL, &err)) == 0)
		goto fail;

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_IFACE_PROFILE_MANAGER, "RegisterProfile");

	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
	if (version)
		g_variant_builder_add(&options, "{sv}", "Version", g_variant_new_uint16(version));
	if (features)
		g_variant_builder_add(&options, "{sv}", "Features", g_variant_new_uint16(features));

	g_dbus_message_set_body(msg, g_variant_new("(osa{sv})", profile_path, uuid, &options));
	g_variant_builder_clear(&options);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	g_hash_table_insert(dbus_object_data_map, hash,
			g_memdup(&dbus_object, sizeof(dbus_object)));

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't register profile: %s", err->message);
		g_dbus_connection_unregister_object(conn, dbus_object.id);
		g_error_free(err);
	}

	return ret;
}

/**
 * Register Bluetooth Hands-Free Audio Profiles.
 *
 * This function also registers deprecated HSP profile. Profiles registration
 * is controlled by the global configuration structure - if none is enabled,
 * this function will do nothing. */
void bluez_register_hfp(void) {
	if (config.enable.hsp_hs)
		bluez_register_profile(BLUETOOTH_UUID_HSP_HS, BA_TRANSPORT_PROFILE_HSP_HS, 0x0, 0x0);
	if (config.enable.hsp_ag)
		bluez_register_profile(BLUETOOTH_UUID_HSP_AG, BA_TRANSPORT_PROFILE_HSP_AG, 0x0, 0x0);
	if (config.enable.hfp_hf)
		bluez_register_profile(BLUETOOTH_UUID_HFP_HF, BA_TRANSPORT_PROFILE_HFP_HF,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_hf);
	if (config.enable.hfp_ag)
		bluez_register_profile(BLUETOOTH_UUID_HFP_AG, BA_TRANSPORT_PROFILE_HFP_AG,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_ag);
}

/**
 * Register to the BlueZ service. */
void bluez_register(void) {

	if (dbus_object_data_map == NULL)
		dbus_object_data_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

	GError *err = NULL;
	GVariantIter *objects = NULL;
	if ((objects = g_dbus_get_managed_objects(config.dbus, BLUEZ_SERVICE, "/", &err)) == NULL) {
		warn("Couldn't get managed objects: %s", err->message);
		g_error_free(err);
		return;
	}

	bool adapters[HCI_MAX_DEV] = { 0 };
	struct ba_adapter *a;

	GVariantIter *interfaces;
	GVariantIter *properties;
	GVariant *value;
	const char *object_path;
	const char *interface;
	const char *property;

	while (g_variant_iter_next(objects, "{&oa{sa{sv}}}", &object_path, &interfaces)) {
		while (g_variant_iter_next(interfaces, "{&sa{sv}}", &interface, &properties)) {
			if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0)
				while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
					if (strcmp(property, "Address") == 0 &&
							bluez_match_dbus_adapter(object_path, g_variant_get_string(value, NULL)))
						/* mark adapter as valid for registration */
						adapters[g_dbus_bluez_object_path_to_hci_dev_id(object_path)] = true;
					g_variant_unref(value);
				}
			g_variant_iter_free(properties);
		}
		g_variant_iter_free(interfaces);
	}
	g_variant_iter_free(objects);

	size_t i;
	for (i = 0; i < HCI_MAX_DEV; i++)
		if (adapters[i] &&
				(a = ba_adapter_new(i, NULL)) != NULL)
			bluez_register_a2dp(a);

	/* HFP has to be registered globally */
	bluez_register_hfp();

}

static void bluez_signal_interfaces_added(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface_, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface_;
	(void)signal;
	(void)userdata;

	struct ba_adapter *a = NULL;

	GVariantIter *interfaces;
	GVariantIter *properties;
	GVariant *value;
	const char *object_path;
	const char *interface;
	const char *property;

	g_variant_get(params, "(&oa{sa{sv}})", &object_path, &interfaces);
	while (g_variant_iter_next(interfaces, "{&sa{sv}}", &interface, &properties)) {
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0)
			while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
				if (strcmp(property, "Address") == 0 &&
						bluez_match_dbus_adapter(object_path, g_variant_get_string(value, NULL)))
					a = ba_adapter_new(g_dbus_bluez_object_path_to_hci_dev_id(object_path), NULL);
				g_variant_unref(value);
			}
		g_variant_iter_free(properties);
	}
	g_variant_iter_free(interfaces);

	if (a != NULL)
		bluez_register_a2dp(a);

	/* HFP has to be registered globally */
	if (strcmp(object_path, "/org/bluez") == 0)
		bluez_register_hfp();

}

static void bluez_signal_interfaces_removed(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface_, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface_;
	(void)signal;
	(void)userdata;

	GVariantIter *interfaces;
	const char *object_path;
	const char *interface;

	g_variant_get(params, "(&oas)", &object_path, &interfaces);
	while (g_variant_iter_next(interfaces, "&s", &interface))
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0) {

			int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);
			struct ba_adapter *a;

			if ((a = ba_adapter_lookup(hci_dev_id)) != NULL)
				ba_adapter_free(a);

		}
	g_variant_iter_free(interfaces);

}

static void bluez_signal_device_changed(GDBusConnection *conn, const gchar *sender,
		const gchar *device_path, const gchar *interface_, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface_;
	(void)userdata;

	const gchar *signature = g_variant_get_type_string(params);
	if (strcmp(signature, "(sa{sv}as)") != 0) {
		error("Invalid signature for %s: %s != %s", signal, signature, "(sa{sv}as)");
		return;
	}

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(device_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL) {
		error("Adapter not available: %s", device_path);
		return;
	}

	GVariantIter *properties = NULL;
	const char *interface;
	const char *property;
	GVariant *value;

	pthread_mutex_lock(&a->devices_mutex);

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL)
		/* If we can not lookup device, it might not be a fail. The properties
		 * changed signal is emitted for every BT device, not only for devices
		 * associated with media transport. */
		goto final;

	g_variant_get(params, "(&sa{sv}as)", &interface, &properties, NULL);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {

		if (strcmp(property, "Alias") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
				warn("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "s");
				goto fail_prop;
			}

			ba_device_set_name(d, g_variant_get_string(value, NULL));

		}

fail_prop:
		g_variant_unref(value);
	}
	g_variant_iter_free(properties);

final:
	pthread_mutex_unlock(&a->devices_mutex);
}

static void bluez_signal_transport_changed(GDBusConnection *conn, const gchar *sender,
		const gchar *transport_path, const gchar *interface_, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface_;
	(void)userdata;

	const gchar *signature = g_variant_get_type_string(params);
	if (strcmp(signature, "(sa{sv}as)") != 0) {
		error("Invalid signature for %s: %s != %s", signal, signature, "(sa{sv}as)");
		return;
	}

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(transport_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL) {
		error("Adapter not available: %s", transport_path);
		return;
	}

	GVariantIter *properties = NULL;
	const char *interface;
	const char *property;
	GVariant *value;

	pthread_mutex_lock(&a->devices_mutex);

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(transport_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL) {
		error("Device not available: %s", transport_path);
		goto final;
	}

	if ((t = ba_transport_lookup(d, transport_path)) == NULL) {
		error("Transport not available: %s", transport_path);
		goto final;
	}

	g_variant_get(params, "(&sa{sv}as)", &interface, &properties, NULL);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
		debug("Signal: %s: %s: %s", signal, interface, property);

		if (strcmp(property, "State") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
				warn("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "s");
				goto fail_prop;
			}

			bluez_a2dp_set_transport_state(t, g_variant_get_string(value, NULL));

		}
		else if (strcmp(property, "Delay") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				warn("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "q");
				goto fail_prop;
			}

			t->a2dp.delay = g_variant_get_uint16(value);

		}
		else if (strcmp(property, "Volume") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				warn("Invalid argument type for %s: %s != %s", property,
						g_variant_get_type_string(value), "q");
				goto fail_prop;
			}

			/* received volume is in range [0, 127]*/
			t->a2dp.ch1_volume = t->a2dp.ch2_volume = g_variant_get_uint16(value);

			bluealsa_ctl_send_event(a->ctl, BA_EVENT_VOLUME_CHANGED, &d->addr,
					BA_PCM_TYPE_A2DP | (t->type.profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE ?
						BA_PCM_STREAM_PLAYBACK : BA_PCM_STREAM_CAPTURE));

		}

fail_prop:
		g_variant_unref(value);
	}
	g_variant_iter_free(properties);

final:
	pthread_mutex_unlock(&a->devices_mutex);
}

/**
 * Subscribe to BlueZ related signals.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_subscribe_signals(void) {

	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			"org.freedesktop.DBus.ObjectManager", "InterfacesAdded", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_interfaces_added, NULL, NULL);
	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			"org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_interfaces_removed, NULL, NULL);

	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			"org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, BLUEZ_IFACE_DEVICE,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_device_changed, NULL, NULL);
	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			"org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, BLUEZ_IFACE_MEDIA_TRANSPORT,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_transport_changed, NULL, NULL);

	return 0;
}
