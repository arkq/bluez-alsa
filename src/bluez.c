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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gunixfdlist.h>

#include "a2dp-codecs.h"
#include "bluealsa.h"
#include "bluez-a2dp.h"
#include "bluez-iface.h"
#include "ctl.h"
#include "transport.h"
#include "utils.h"
#include "shared/log.h"


/**
 * Get D-Bus object reference count for given profile. */
static int bluez_get_dbus_object_count(
		enum bluetooth_profile profile,
		uint16_t codec) {

	GHashTableIter iter;
	struct ba_dbus_object *obj;
	int count = 0;

	g_hash_table_iter_init(&iter, config.dbus_objects);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&obj))
		if (obj->profile == profile && obj->codec == codec && obj->connected)
			count++;

	return count;
}

/**
 * Get device associated with given D-Bus object path. */
struct ba_device *bluez_get_device(const char *path) {

#if DEBUG
	/* make sure that the device mutex is acquired */
	g_assert(pthread_mutex_trylock(&config.devices_mutex) == EBUSY);
#endif

	struct ba_device *d;
	char name[sizeof(d->name)];
	GVariant *property;
	bdaddr_t addr;

	if ((d = bluealsa_device_lookup(path)) != NULL)
		return d;

	g_dbus_device_path_to_bdaddr(path, &addr);
	ba2str(&addr, name);

	/* get local (user editable) Bluetooth device name */
	if ((property = g_dbus_get_property(config.dbus, BLUEZ_SERVICE, path,
					BLUEZ_IFACE_DEVICE, "Alias")) != NULL) {
		strncpy(name, g_variant_get_string(property, NULL), sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
		g_variant_unref(property);
	}

	d = device_new(config.hci_dev.dev_id, &addr, name);
	bluealsa_device_insert(path, d);
	return d;
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
		return transport_set_state(t, TRANSPORT_IDLE);
	else if (strcmp(state, BLUEZ_TRANSPORT_STATE_PENDING) == 0)
		return transport_set_state(t, TRANSPORT_PENDING);
	else if (strcmp(state, BLUEZ_TRANSPORT_STATE_ACTIVE) == 0)
		return transport_set_state(t, TRANSPORT_ACTIVE);

	warn("Invalid state: %s", state);
	return -1;
}

static void bluez_endpoint_select_configuration(GDBusMethodInvocation *inv, void *userdata) {

	const char *path = g_dbus_method_invocation_get_object_path(inv);
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
		debug("Endpoint path not supported: %s", path);
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
	const gchar *path = g_dbus_method_invocation_get_object_path(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	const struct bluez_a2dp_codec *codec = userdata;
	bool devpool_mutex_locked = false;
	struct ba_transport *t;
	struct ba_device *d;

	const int profile = g_dbus_object_path_to_profile(path);
	const uint16_t codec_id = codec->id;

	char *device = NULL, *state = NULL;
	uint8_t *configuration = NULL;
	uint16_t volume = 127;
	uint16_t delay = 150;
	size_t size = 0;
	int ret = 0;

	const char *transport;
	GVariantIter *properties;
	GVariant *value = NULL;
	const char *key;

	g_variant_get(params, "(&oa{sv})", &transport, &properties);
	while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {

		if (strcmp(key, "Device") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "o");
				goto fail;
			}

			device = g_variant_dup_string(value, NULL);

		}
		else if (strcmp(key, "UUID") == 0) {
		}
		else if (strcmp(key, "Codec") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "y");
				goto fail;
			}

			if ((codec_id & 0xFF) != g_variant_get_byte(value)) {
				error("Invalid configuration: %s", "Codec mismatch");
				goto fail;
			}

		}
		else if (strcmp(key, "Configuration") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING)) {
				error("Invalid argument type for %s: %s != %s", key,
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
		else if (strcmp(key, "State") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "s");
				goto fail;
			}

			state = g_variant_dup_string(value, NULL);

		}
		else if (strcmp(key, "Delay") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "q");
				goto fail;
			}

			delay = g_variant_get_uint16(value);

		}
		else if (strcmp(key, "Volume") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "q");
				goto fail;
			}

			/* received volume is in range [0, 127]*/
			volume = g_variant_get_uint16(value);

		}

		g_variant_unref(value);
		value = NULL;
	}

	/* we are going to modify the devices hash-map */
	bluealsa_devpool_mutex_lock();
	devpool_mutex_locked = true;

	if (transport_lookup(config.devices, transport) != NULL) {
		error("Transport already configured: %s", transport);
		goto fail;
	}

	/* get the device structure for obtained device path */
	if ((d = bluez_get_device(device)) == NULL) {
		error("Couldn't get device: %s", strerror(errno));
		goto fail;
	}

	/* Create a new transport with a human-readable name. Since the transport
	 * name can not be obtained from the client, we will use a fall-back one. */
	if ((t = transport_new_a2dp(d, sender, transport, profile, codec_id,
					configuration, size)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->a2dp.ch1_volume = volume;
	t->a2dp.ch2_volume = volume;
	t->a2dp.delay = delay;

	debug("%s (%s) configured for device %s",
			bluetooth_profile_to_string(profile),
			bluetooth_a2dp_codec_to_string(codec_id),
			batostr_(&d->addr));
	debug("Configuration: channels: %u, sampling: %u",
			transport_get_channels(t), transport_get_sampling(t));

	bluez_a2dp_set_transport_state(t, state);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to set configuration");
	ret = -1;

final:
	if (devpool_mutex_locked)
		bluealsa_devpool_mutex_unlock();
	g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
	g_free(device);
	g_free(configuration);
	g_free(state);
	return ret;
}

static void bluez_endpoint_clear_configuration(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	const char *transport;

	g_variant_get(params, "(&o)", &transport);

	bluealsa_devpool_mutex_lock();
	transport_remove(config.devices, transport);
	bluealsa_devpool_mutex_unlock();

	g_object_unref(inv);
}

static void bluez_endpoint_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusConnection *conn = g_dbus_method_invocation_get_connection(inv);
	const char *path = g_dbus_method_invocation_get_object_path(inv);
	gpointer hash = GINT_TO_POINTER(g_str_hash(path));
	struct ba_dbus_object *obj;

	debug("Releasing endpoint: %s", path);

	if ((obj = g_hash_table_lookup(config.dbus_objects, hash)) != NULL) {
		g_dbus_connection_unregister_object(conn, obj->id);
		g_hash_table_remove(config.dbus_objects, hash);
	}

	g_object_unref(inv);
}

static void bluez_endpoint_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface;
	(void)params;

	debug("Endpoint method call: %s.%s()", interface, method);

	gpointer hash = GINT_TO_POINTER(g_str_hash(path));
	struct ba_dbus_object *obj;

	if (strcmp(method, "SelectConfiguration") == 0)
		bluez_endpoint_select_configuration(invocation, userdata);
	else if (strcmp(method, "SetConfiguration") == 0) {
		if (bluez_endpoint_set_configuration(invocation, userdata) == 0) {
			obj = g_hash_table_lookup(config.dbus_objects, hash);
			obj->connected = true;
			bluez_register_a2dp();
		}
	}
	else if (strcmp(method, "ClearConfiguration") == 0) {
		bluez_endpoint_clear_configuration(invocation, userdata);
		obj = g_hash_table_lookup(config.dbus_objects, hash);
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

static const GDBusInterfaceVTable endpoint_vtable = {
	.method_call = bluez_endpoint_method_call,
};

/**
 * Register A2DP endpoint.
 *
 * @param uuid
 * @param profile
 * @param codec
 * @return On success this function returns 0. Otherwise -1 is returned. */
static int bluez_register_a2dp_endpoint(
		const char *uuid,
		enum bluetooth_profile profile,
		const struct bluez_a2dp_codec *codec) {

	gchar *path = g_strdup_printf("%s/%d",
		g_dbus_get_profile_object_path(profile, codec->id),
		bluez_get_dbus_object_count(profile, codec->id) + 1);
	gpointer hash = GINT_TO_POINTER(g_str_hash(path));

	if (g_hash_table_contains(config.dbus_objects, hash)) {
		debug("Endpoint already registered: %s", path);
		g_free(path);
		return 0;
	}

	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	gchar *dev = NULL;
	int ret = 0;
	size_t i;

	struct ba_dbus_object dbus_object = {
		.profile = profile,
		.codec = codec->id,
	};

	debug("Registering endpoint: %s", path);
	if ((dbus_object.id = g_dbus_connection_register_object(conn, path,
					(GDBusInterfaceInfo *)&bluez_iface_endpoint, &endpoint_vtable,
					(void *)codec, endpoint_free, &err)) == 0)
		goto fail;

	dev = g_strdup_printf("/org/bluez/%s", config.hci_dev.name);
	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, dev,
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

	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", path, &properties));
	g_variant_builder_clear(&properties);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	g_hash_table_insert(config.dbus_objects, hash,
			g_memdup(&dbus_object, sizeof(dbus_object)));

	goto final;

fail:
	ret = -1;

final:
	g_free(path);
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (dev != NULL)
		g_free(dev);
	if (err != NULL) {
		warn("Couldn't register endpoint: %s", err->message);
		g_dbus_connection_unregister_object(conn, dbus_object.id);
		g_error_free(err);
	}

	return ret;
}

/**
 * Register A2DP endpoints. */
void bluez_register_a2dp(void) {

	const struct bluez_a2dp_codec **cc = config.a2dp.codecs;

	while (*cc != NULL) {
		const struct bluez_a2dp_codec *c = *cc++;
		switch (c->dir) {
		case BLUEZ_A2DP_SOURCE:
			if (config.enable.a2dp_source)
				bluez_register_a2dp_endpoint(BLUETOOTH_UUID_A2DP_SOURCE, BLUETOOTH_PROFILE_A2DP_SOURCE, c);
			break;
		case BLUEZ_A2DP_SINK:
			if (config.enable.a2dp_sink)
				bluez_register_a2dp_endpoint(BLUETOOTH_UUID_A2DP_SINK, BLUETOOTH_PROFILE_A2DP_SINK, c);
			break;
		}
	}

}

static void bluez_profile_new_connection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	const gchar *sender = g_dbus_method_invocation_get_sender(inv);
	const gchar *path = g_dbus_method_invocation_get_object_path(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	bool devpool_mutex_locked = false;
	struct ba_transport *t;
	struct ba_device *d;

	const int profile = g_dbus_object_path_to_profile(path);

	GVariantIter *properties;
	GUnixFDList *fd_list;
	const char *device;
	GError *err = NULL;
	int fd;

	g_variant_get(params, "(&oha{sv})", &device, &fd, &properties);

	fd_list = g_dbus_message_get_unix_fd_list(msg);
	if ((fd = g_unix_fd_list_get(fd_list, 0, &err)) == -1) {
		error("Couldn't obtain RFCOMM socket: %s", err->message);
		goto fail;
	}

	/* we are going to modify the devices hash-map */
	bluealsa_devpool_mutex_lock();
	devpool_mutex_locked = true;

	if ((d = bluez_get_device(device)) == NULL) {
		error("Couldn't get device: %s", strerror(errno));
		goto fail;
	}

	if ((t = transport_new_rfcomm(d, sender, device, profile)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->bt_fd = fd;

	debug("%s configured for device %s",
			bluetooth_profile_to_string(profile), batostr_(&d->addr));

	transport_set_state(t, TRANSPORT_ACTIVE);
	transport_set_state(t->rfcomm.sco, TRANSPORT_ACTIVE);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to connect profile");

final:
	if (devpool_mutex_locked)
		bluealsa_devpool_mutex_unlock();
	g_variant_iter_free(properties);
	if (err != NULL)
		g_error_free(err);
}

static void bluez_profile_request_disconnection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	const char *device;

	g_variant_get(params, "(&o)", &device);

	bluealsa_devpool_mutex_lock();
	transport_remove(config.devices, device);
	bluealsa_devpool_mutex_unlock();

	g_object_unref(inv);
}

static void bluez_profile_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusConnection *conn = g_dbus_method_invocation_get_connection(inv);
	const char *path = g_dbus_method_invocation_get_object_path(inv);
	gpointer hash = GINT_TO_POINTER(g_str_hash(path));
	struct ba_dbus_object *obj;

	debug("Releasing profile: %s", path);

	if ((obj = g_hash_table_lookup(config.dbus_objects, hash)) != NULL) {
		g_dbus_connection_unregister_object(conn, obj->id);
		g_hash_table_remove(config.dbus_objects, hash);
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

static const GDBusInterfaceVTable profile_vtable = {
	.method_call = bluez_profile_method_call,
};

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
		enum bluetooth_profile profile,
		uint16_t version,
		uint16_t features) {

	const char *path = g_dbus_get_profile_object_path(profile, -1);
	gpointer hash = GINT_TO_POINTER(g_str_hash(path));

	if (g_hash_table_contains(config.dbus_objects, hash)) {
		debug("Profile already registered: %s", path);
		return 0;
	}

	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	struct ba_dbus_object dbus_object = {
		.profile = profile,
		.codec = 0,
	};

	debug("Registering profile: %s", path);
	if ((dbus_object.id = g_dbus_connection_register_object(conn, path,
					(GDBusInterfaceInfo *)&bluez_iface_profile, &profile_vtable,
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

	g_dbus_message_set_body(msg, g_variant_new("(osa{sv})", path, uuid, &options));
	g_variant_builder_clear(&options);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	g_hash_table_insert(config.dbus_objects, hash,
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
		bluez_register_profile(BLUETOOTH_UUID_HSP_HS, BLUETOOTH_PROFILE_HSP_HS, 0x0, 0x0);
	if (config.enable.hsp_ag)
		bluez_register_profile(BLUETOOTH_UUID_HSP_AG, BLUETOOTH_PROFILE_HSP_AG, 0x0, 0x0);
	if (config.enable.hfp_hf)
		bluez_register_profile(BLUETOOTH_UUID_HFP_HF, BLUETOOTH_PROFILE_HFP_HF,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_hf);
	if (config.enable.hfp_ag)
		bluez_register_profile(BLUETOOTH_UUID_HFP_AG, BLUETOOTH_PROFILE_HFP_AG,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_ag);
}

static void bluez_signal_interfaces_added(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)signal;
	(void)userdata;

	char *device_path = g_strdup_printf("/org/bluez/%s", config.hci_dev.name);
	GVariantIter *interfaces;
	const char *object;

	g_variant_get(params, "(&oa{sa{sv}})", &object, &interfaces);

	if (strcmp(object, device_path) == 0)
		bluez_register_a2dp();
	if (strcmp(object, "/org/bluez") == 0)
		bluez_register_hfp();

	g_variant_iter_free(interfaces);
	g_free(device_path);
}

static void bluez_signal_transport_changed(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface;
	(void)userdata;

	const gchar *signature = g_variant_get_type_string(params);
	bool devpool_mutex_locked = false;
	GVariantIter *properties = NULL;
	GVariantIter *unknown = NULL;
	GVariant *value = NULL;
	struct ba_transport *t;
	const char *iface;
	const char *key;

	if (strcmp(signature, "(sa{sv}as)") != 0) {
		error("Invalid signature for %s: %s != %s", signal, signature, "(sa{sv}as)");
		goto fail;
	}

	bluealsa_devpool_mutex_lock();
	devpool_mutex_locked = true;

	if ((t = transport_lookup(config.devices, path)) == NULL) {
		error("Transport not available: %s", path);
		goto fail;
	}

	g_variant_get(params, "(&sa{sv}as)", &iface, &properties, &unknown);
	while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {
		debug("Signal: %s: %s: %s", signal, iface, key);

		if (strcmp(key, "State") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "s");
				goto fail;
			}

			bluez_a2dp_set_transport_state(t, g_variant_get_string(value, NULL));

		}
		else if (strcmp(key, "Delay") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "q");
				goto fail;
			}

			t->a2dp.delay = g_variant_get_uint16(value);

		}
		else if (strcmp(key, "Volume") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "q");
				goto fail;
			}

			/* received volume is in range [0, 127]*/
			t->a2dp.ch1_volume = t->a2dp.ch2_volume = g_variant_get_uint16(value);

			bluealsa_ctl_send_event(BA_EVENT_VOLUME_CHANGED, &t->device->addr,
					BA_PCM_TYPE_A2DP | (t->profile == BLUETOOTH_PROFILE_A2DP_SOURCE ?
						BA_PCM_STREAM_PLAYBACK : BA_PCM_STREAM_CAPTURE));

		}

		g_variant_unref(value);
		value = NULL;
	}

fail:
	if (devpool_mutex_locked)
		bluealsa_devpool_mutex_unlock();
	if (properties != NULL)
		g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
}

/**
 * Subscribe to BlueZ related signals.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_subscribe_signals(void) {

	GDBusConnection *conn = config.dbus;

	/* Note, that we do not have to subscribe for the interfaces remove signal,
	 * because prior to removal, BlueZ will emit appropriate "clear" signal. */
	g_dbus_connection_signal_subscribe(conn, BLUEZ_SERVICE, "org.freedesktop.DBus.ObjectManager",
			"InterfacesAdded", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			/* TODO: Use arg0 filtering, but is seems it doesn't work... */
			/* "InterfacesAdded", NULL, "/org/bluez/hci0", G_DBUS_SIGNAL_FLAGS_NONE, */
			bluez_signal_interfaces_added, NULL, NULL);

	g_dbus_connection_signal_subscribe(conn, BLUEZ_SERVICE, "org.freedesktop.DBus.Properties",
			"PropertiesChanged", NULL, BLUEZ_IFACE_MEDIA_TRANSPORT, G_DBUS_SIGNAL_FLAGS_NONE,
			bluez_signal_transport_changed, NULL, NULL);

	return 0;
}
