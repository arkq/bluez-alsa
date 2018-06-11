/*
 * BlueALSA - bluez.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
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
#include "transport.h"
#include "utils.h"
#include "shared/log.h"


/**
 * Get D-Bus object reference count for given profile. */
static int bluez_get_dbus_object_count(enum bluetooth_profile profile, uint16_t codec) {

	GHashTableIter iter;
	struct ba_dbus_object *obj;
	int count = 0;

	g_hash_table_iter_init(&iter, config.dbus_objects);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&obj))
		if (obj->profile == profile && obj->codec == codec && obj->connected)
			count++;

	return count;
}

static void bluez_endpoint_select_configuration(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	const char *path = g_dbus_method_invocation_get_object_path(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);

	const uint8_t *data;
	uint8_t *capabilities;
	size_t size = 0;

	params = g_variant_get_child_value(params, 0);
	data = g_variant_get_fixed_array(params, &size, sizeof(uint8_t));
	capabilities = g_memdup(data, size);
	g_variant_unref(params);

	switch (g_dbus_object_path_to_a2dp_codec(path)) {
	case A2DP_CODEC_SBC: {

		if (size != sizeof(a2dp_sbc_t)) {
			error("Invalid capabilities size: %zu != %zu", size, sizeof(a2dp_sbc_t));
			goto fail;
		}

		a2dp_sbc_t *cap = (a2dp_sbc_t *)capabilities;

		if (config.a2dp_force_44100 &&
				cap->frequency & SBC_SAMPLING_FREQ_44100)
			cap->frequency = SBC_SAMPLING_FREQ_44100;
		else if (cap->frequency & SBC_SAMPLING_FREQ_48000)
			cap->frequency = SBC_SAMPLING_FREQ_48000;
		else if (cap->frequency & SBC_SAMPLING_FREQ_44100)
			cap->frequency = SBC_SAMPLING_FREQ_44100;
		else if (cap->frequency & SBC_SAMPLING_FREQ_32000)
			cap->frequency = SBC_SAMPLING_FREQ_32000;
		else if (cap->frequency & SBC_SAMPLING_FREQ_16000)
			cap->frequency = SBC_SAMPLING_FREQ_16000;
		else {
			error("No supported sampling frequencies: %#x", cap->frequency);
			goto fail;
		}

		if (config.a2dp_force_mono &&
				cap->channel_mode & SBC_CHANNEL_MODE_MONO)
			cap->channel_mode = SBC_CHANNEL_MODE_MONO;
		else if (cap->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
			cap->channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
		else if (cap->channel_mode & SBC_CHANNEL_MODE_STEREO)
			cap->channel_mode = SBC_CHANNEL_MODE_STEREO;
		else if (cap->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
			cap->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
		else if (cap->channel_mode & SBC_CHANNEL_MODE_MONO)
			cap->channel_mode = SBC_CHANNEL_MODE_MONO;
		else {
			error("No supported channel modes: %#x", cap->channel_mode);
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
		cap->min_bitpool = MAX(MIN_BITPOOL, cap->min_bitpool);
		cap->max_bitpool = MIN(bitpool, cap->max_bitpool);

		break;
	}

#if ENABLE_AAC
	case A2DP_CODEC_MPEG24: {

		if (size != sizeof(a2dp_aac_t)) {
			error("Invalid capabilities size: %zu != %zu", size, sizeof(a2dp_aac_t));
			goto fail;
		}

		a2dp_aac_t *cap = (a2dp_aac_t *)capabilities;

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

		unsigned int sampling = AAC_GET_FREQUENCY(*cap);
		if (config.a2dp_force_44100 &&
				sampling & AAC_SAMPLING_FREQ_44100)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_44100);
		else if (sampling & AAC_SAMPLING_FREQ_96000)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_96000);
		else if (sampling & AAC_SAMPLING_FREQ_88200)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_88200);
		else if (sampling & AAC_SAMPLING_FREQ_64000)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_64000);
		else if (sampling & AAC_SAMPLING_FREQ_48000)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_48000);
		else if (sampling & AAC_SAMPLING_FREQ_44100)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_44100);
		else if (sampling & AAC_SAMPLING_FREQ_32000)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_32000);
		else if (sampling & AAC_SAMPLING_FREQ_24000)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_24000);
		else if (sampling & AAC_SAMPLING_FREQ_22050)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_22050);
		else if (sampling & AAC_SAMPLING_FREQ_16000)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_16000);
		else if (sampling & AAC_SAMPLING_FREQ_12000)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_12000);
		else if (sampling & AAC_SAMPLING_FREQ_11025)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_11025);
		else if (sampling & AAC_SAMPLING_FREQ_8000)
			AAC_SET_FREQUENCY(*cap, AAC_SAMPLING_FREQ_8000);
		else {
			error("No supported sampling frequencies: %#x", sampling);
			goto fail;
		}

		if (config.a2dp_force_mono &&
				cap->channels & AAC_CHANNELS_1)
			cap->channels = AAC_CHANNELS_1;
		else if (cap->channels & AAC_CHANNELS_2)
			cap->channels = AAC_CHANNELS_2;
		else if (cap->channels & AAC_CHANNELS_1)
			cap->channels = AAC_CHANNELS_1;
		else {
			error("No supported channels: %#x", cap->channels);
			goto fail;
		}

		break;
	}
#endif

#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX: {

		if (size != sizeof(a2dp_aptx_t)) {
			error("Invalid capabilities size: %zu != %zu", size, sizeof(a2dp_aptx_t));
			goto fail;
		}

		a2dp_aptx_t *cap = (a2dp_aptx_t *)capabilities;

		if (config.a2dp_force_44100 &&
				cap->frequency & APTX_SAMPLING_FREQ_44100)
			cap->frequency = APTX_SAMPLING_FREQ_44100;
		else if (cap->frequency & APTX_SAMPLING_FREQ_48000)
			cap->frequency = APTX_SAMPLING_FREQ_48000;
		else if (cap->frequency & APTX_SAMPLING_FREQ_44100)
			cap->frequency = APTX_SAMPLING_FREQ_44100;
		else if (cap->frequency & APTX_SAMPLING_FREQ_32000)
			cap->frequency = APTX_SAMPLING_FREQ_32000;
		else if (cap->frequency & APTX_SAMPLING_FREQ_16000)
			cap->frequency = APTX_SAMPLING_FREQ_16000;
		else {
			error("No supported sampling frequencies: %#x", cap->frequency);
			goto fail;
		}

		if (cap->channel_mode & APTX_CHANNEL_MODE_STEREO)
			cap->channel_mode = APTX_CHANNEL_MODE_STEREO;
		else {
			error("No supported channel modes: %#x", cap->channel_mode);
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
	(void)userdata;

	const gchar *sender = g_dbus_method_invocation_get_sender(inv);
	const gchar *path = g_dbus_method_invocation_get_object_path(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct ba_transport *t;
	struct ba_device *d;

	const int profile = g_dbus_object_path_to_profile(path);
	const uint16_t codec = g_dbus_object_path_to_a2dp_codec(path);

	const char *transport;
	char *device = NULL, *state = NULL;
	uint8_t *configuration = NULL;
	uint16_t volume = 127;
	uint16_t delay = 150;
	size_t size = 0;
	int ret = 0;

	GVariantIter *properties;
	GVariant *value = NULL;
	const char *key;

	g_variant_get(params, "(&oa{sv})", &transport, &properties);

	if (transport_lookup(config.devices, transport) != NULL) {
		error("Transport already configured: %s", transport);
		goto fail;
	}

	/* read transport properties */
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

			if ((codec & 0xFF) != g_variant_get_byte(value)) {
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

			const guchar *capabilities;

			capabilities = g_variant_get_fixed_array(value, &size, sizeof(uint8_t));
			configuration = g_memdup(capabilities, size);

			switch (codec) {
			case A2DP_CODEC_SBC: {

				if (size != sizeof(a2dp_sbc_t)) {
					error("Invalid configuration: %s", "Invalid size");
					goto fail;
				}

				const a2dp_sbc_t *cap = (a2dp_sbc_t *)capabilities;

				if (cap->frequency != SBC_SAMPLING_FREQ_16000 &&
						cap->frequency != SBC_SAMPLING_FREQ_32000 &&
						cap->frequency != SBC_SAMPLING_FREQ_44100 &&
						cap->frequency != SBC_SAMPLING_FREQ_48000) {
					error("Invalid configuration: %s", "Invalid sampling frequency");
					goto fail;
				}

				if (cap->channel_mode != SBC_CHANNEL_MODE_MONO &&
						cap->channel_mode != SBC_CHANNEL_MODE_DUAL_CHANNEL &&
						cap->channel_mode != SBC_CHANNEL_MODE_STEREO &&
						cap->channel_mode != SBC_CHANNEL_MODE_JOINT_STEREO) {
					error("Invalid configuration: %s", "Invalid channel mode");
					goto fail;
				}

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

#if ENABLE_AAC
			case A2DP_CODEC_MPEG24: {

				if (size != sizeof(a2dp_aac_t)) {
					error("Invalid configuration: %s", "Invalid size");
					goto fail;
				}

				const a2dp_aac_t *cap = (a2dp_aac_t *)capabilities;

				if (cap->object_type != AAC_OBJECT_TYPE_MPEG2_AAC_LC &&
						cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_LC &&
						cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_LTP &&
						cap->object_type != AAC_OBJECT_TYPE_MPEG4_AAC_SCA) {
					error("Invalid configuration: %s", "Invalid object type");
					goto fail;
				}

				unsigned int sampling = AAC_GET_FREQUENCY(*cap);
				if (sampling != AAC_SAMPLING_FREQ_8000 &&
						sampling != AAC_SAMPLING_FREQ_11025 &&
						sampling != AAC_SAMPLING_FREQ_12000 &&
						sampling != AAC_SAMPLING_FREQ_16000 &&
						sampling != AAC_SAMPLING_FREQ_22050 &&
						sampling != AAC_SAMPLING_FREQ_24000 &&
						sampling != AAC_SAMPLING_FREQ_32000 &&
						sampling != AAC_SAMPLING_FREQ_44100 &&
						sampling != AAC_SAMPLING_FREQ_48000 &&
						sampling != AAC_SAMPLING_FREQ_64000 &&
						sampling != AAC_SAMPLING_FREQ_88200 &&
						sampling != AAC_SAMPLING_FREQ_96000) {
					error("Invalid configuration: %s", "Invalid sampling frequency");
					goto fail;
				}

				if (cap->channels != AAC_CHANNELS_1 &&
						cap->channels != AAC_CHANNELS_2) {
					error("Invalid configuration: %s", "Invalid channels");
					goto fail;
				}

				break;
			}
#endif

#if ENABLE_APTX
			case A2DP_CODEC_VENDOR_APTX: {

				if (size != sizeof(a2dp_aptx_t)) {
					error("Invalid configuration: %s", "Invalid size");
					goto fail;
				}

				a2dp_aptx_t *cap = (a2dp_aptx_t *)capabilities;

				if (cap->frequency != APTX_SAMPLING_FREQ_16000 &&
						cap->frequency != APTX_SAMPLING_FREQ_32000 &&
						cap->frequency != APTX_SAMPLING_FREQ_44100 &&
						cap->frequency != APTX_SAMPLING_FREQ_48000) {
					error("Invalid configuration: %s", "Invalid sampling frequency");
					goto fail;
				}

				if (cap->channel_mode != APTX_CHANNEL_MODE_STEREO) {
					error("Invalid configuration: %s", "Invalid channel mode");
					goto fail;
				}

				break;
			}
#endif

			default:
				error("Invalid configuration: %s", "Unsupported codec");
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
	pthread_mutex_lock(&config.devices_mutex);

	/* get the device structure for obtained device path */
	if ((d = device_get(config.devices, device)) == NULL) {
		error("Couldn't get device: %s", strerror(errno));
		goto fail;
	}

	/* Create a new transport with a human-readable name. Since the transport
	 * name can not be obtained from the client, we will use a fall-back one. */
	if ((t = transport_new_a2dp(d, sender, transport, profile, codec,
					configuration, size)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->a2dp.ch1_volume = volume;
	t->a2dp.ch2_volume = volume;
	t->a2dp.delay = delay;

	debug("%s configured for device %s",
			bluetooth_profile_to_string(profile, codec), batostr_(&d->addr));
	debug("Configuration: channels: %u, sampling: %u",
			transport_get_channels(t), transport_get_sampling(t));

	transport_set_state_from_string(t, state);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to set configuration");
	ret = -1;

final:
	pthread_mutex_unlock(&config.devices_mutex);
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

	pthread_mutex_lock(&config.devices_mutex);

	g_variant_get(params, "(&o)", &transport);
	transport_remove(config.devices, transport);

	pthread_mutex_unlock(&config.devices_mutex);
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

	struct ba_dbus_object *obj;

	debug("Endpoint method call: %s.%s()", interface, method);

	gpointer hash = GINT_TO_POINTER(g_str_hash(path));
	obj = g_hash_table_lookup(config.dbus_objects, hash);

	if (strcmp(method, "SelectConfiguration") == 0)
		bluez_endpoint_select_configuration(invocation, userdata);
	else if (strcmp(method, "SetConfiguration") == 0) {
		if (bluez_endpoint_set_configuration(invocation, userdata) == 0) {
			obj->connected = true;
			bluez_register_a2dp();
		}
	}
	else if (strcmp(method, "ClearConfiguration") == 0) {
		bluez_endpoint_clear_configuration(invocation, userdata);
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
 * @param configuration
 * @param configuration_size
 * @return On success this function returns 0. Otherwise -1 is returned. */
static int bluez_register_a2dp_endpoint(
		const char *uuid,
		enum bluetooth_profile profile,
		uint16_t codec,
		const void *configuration,
		size_t configuration_size) {

	gchar *path = g_strdup_printf("%s/%d",
		g_dbus_get_profile_object_path(profile, codec),
		bluez_get_dbus_object_count(profile, codec) + 1);
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
		.codec = codec,
	};

	debug("Registering endpoint: %s", path);
	if ((dbus_object.id = g_dbus_connection_register_object(conn, path,
					(GDBusInterfaceInfo *)&bluez_iface_endpoint, &endpoint_vtable,
					NULL, endpoint_free, &err)) == 0)
		goto fail;

	dev = g_strdup_printf("/org/bluez/%s", config.hci_dev.name);
	msg = g_dbus_message_new_method_call("org.bluez", dev,
			"org.bluez.Media1", "RegisterEndpoint");

	GVariantBuilder caps;
	GVariantBuilder properties;

	g_variant_builder_init(&caps, G_VARIANT_TYPE("ay"));
	g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));

	for (i = 0; i < configuration_size; i++)
		g_variant_builder_add(&caps, "y", ((uint8_t *)configuration)[i]);

	g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(uuid));
	g_variant_builder_add(&properties, "{sv}", "Codec", g_variant_new_byte(codec));
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
	if (config.enable.a2dp_source) {
#if ENABLE_AAC
		bluez_register_a2dp_endpoint(BLUETOOTH_UUID_A2DP_SOURCE, BLUETOOTH_PROFILE_A2DP_SOURCE,
				A2DP_CODEC_MPEG24, &bluez_a2dp_aac, sizeof(bluez_a2dp_aac));
#endif
#if ENABLE_APTX
		bluez_register_a2dp_endpoint(BLUETOOTH_UUID_A2DP_SOURCE, BLUETOOTH_PROFILE_A2DP_SOURCE,
				A2DP_CODEC_VENDOR_APTX, &bluez_a2dp_aptx, sizeof(bluez_a2dp_aptx));
#endif
		bluez_register_a2dp_endpoint(BLUETOOTH_UUID_A2DP_SOURCE, BLUETOOTH_PROFILE_A2DP_SOURCE,
				A2DP_CODEC_SBC, &bluez_a2dp_sbc, sizeof(bluez_a2dp_sbc));
	}
	if (config.enable.a2dp_sink) {
#if ENABLE_AAC
		bluez_register_a2dp_endpoint(BLUETOOTH_UUID_A2DP_SINK, BLUETOOTH_PROFILE_A2DP_SINK,
				A2DP_CODEC_MPEG24, &bluez_a2dp_aac, sizeof(bluez_a2dp_aac));
#endif
		bluez_register_a2dp_endpoint(BLUETOOTH_UUID_A2DP_SINK, BLUETOOTH_PROFILE_A2DP_SINK,
				A2DP_CODEC_SBC, &bluez_a2dp_sbc, sizeof(bluez_a2dp_sbc));
	}
}

static void bluez_profile_new_connection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	const gchar *sender = g_dbus_method_invocation_get_sender(inv);
	const gchar *path = g_dbus_method_invocation_get_object_path(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
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

	if ((d = device_get(config.devices, device)) == NULL) {
		error("Couldn't get device: %s", strerror(errno));
		goto fail;
	}

	if ((t = transport_new_rfcomm(d, sender, device, profile)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->bt_fd = fd;
	t->release = transport_release_bt_rfcomm;

	debug("%s configured for device %s",
			bluetooth_profile_to_string(profile, -1), batostr_(&d->addr));

	transport_set_state(t, TRANSPORT_ACTIVE);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to connect profile");

final:
	g_variant_iter_free(properties);
	if (err != NULL)
		g_error_free(err);
}

static void bluez_profile_request_disconnection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	const char *device;

	pthread_mutex_lock(&config.devices_mutex);

	g_variant_get(params, "(&o)", &device);
	transport_remove(config.devices, device);

	pthread_mutex_unlock(&config.devices_mutex);

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

void profile_free(gpointer data) {
	(void)data;
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
					NULL, profile_free, &err)) == 0)
		goto fail;

	msg = g_dbus_message_new_method_call("org.bluez", "/org/bluez",
			"org.bluez.ProfileManager1", "RegisterProfile");

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

	if ((t = transport_lookup(config.devices, path)) == NULL) {
		error("Transport not available: %s", path);
		goto fail;
	}

	g_variant_get(params, "(&sa{sv}as)", &iface, &properties, &unknown);
	debug("Signal: %s: %s", signal, iface);

	while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {

		if (strcmp(key, "State") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "s");
				goto fail;
			}

			transport_set_state_from_string(t, g_variant_get_string(value, NULL));

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

		}

		g_variant_unref(value);
		value = NULL;
	}

fail:
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
	g_dbus_connection_signal_subscribe(conn, "org.bluez", "org.freedesktop.DBus.ObjectManager",
			"InterfacesAdded", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			/* TODO: Use arg0 filtering, but is seems it doesn't work... */
			/* "InterfacesAdded", NULL, "/org/bluez/hci0", G_DBUS_SIGNAL_FLAGS_NONE, */
			bluez_signal_interfaces_added, NULL, NULL);

	g_dbus_connection_signal_subscribe(conn, "org.bluez", "org.freedesktop.DBus.Properties",
			"PropertiesChanged", NULL, "org.bluez.MediaTransport1", G_DBUS_SIGNAL_FLAGS_NONE,
			bluez_signal_transport_changed, NULL, NULL);

	return 0;
}
