/*
 * BlueALSA - bluez.c
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
#include "bluez-iface.h"
#include "log.h"
#include "transport.h"
#include "utils.h"


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

	if (strcmp(path, BLUEZ_ENDPOINT_A2DP_SBC_SOURCE) == 0 ||
			strcmp(path, BLUEZ_ENDPOINT_A2DP_SBC_SINK) == 0) {

		if (size != sizeof(a2dp_sbc_t)) {
			error("Invalid capabilities size: %zu != %zu", size, sizeof(a2dp_sbc_t));
			goto fail;
		}

		a2dp_sbc_t *cap = (a2dp_sbc_t *)capabilities;

		if (cap->frequency & SBC_SAMPLING_FREQ_48000)
			cap->frequency = SBC_SAMPLING_FREQ_48000;
		else if (cap->frequency & SBC_SAMPLING_FREQ_44100)
			cap->frequency = SBC_SAMPLING_FREQ_44100;
		else if (cap->frequency & SBC_SAMPLING_FREQ_32000)
			cap->frequency = SBC_SAMPLING_FREQ_32000;
		else if (cap->frequency & SBC_SAMPLING_FREQ_16000)
			cap->frequency = SBC_SAMPLING_FREQ_16000;
		else {
			error("No supported frequencies: %u", cap->frequency);
			goto fail;
		}

		if (cap->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
			cap->channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
		else if (cap->channel_mode & SBC_CHANNEL_MODE_STEREO)
			cap->channel_mode = SBC_CHANNEL_MODE_STEREO;
		else if (cap->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
			cap->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
		else if (cap->channel_mode & SBC_CHANNEL_MODE_MONO) {
			cap->channel_mode = SBC_CHANNEL_MODE_MONO;
		} else {
			error("No supported channel modes: %u", cap->channel_mode);
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
			error("No supported block lengths: %u", cap->block_length);
			goto fail;
		}

		if (cap->subbands & SBC_SUBBANDS_8)
			cap->subbands = SBC_SUBBANDS_8;
		else if (cap->subbands & SBC_SUBBANDS_4)
			cap->subbands = SBC_SUBBANDS_4;
		else {
			error("No supported subbands: %u", cap->subbands);
			goto fail;
		}

		if (cap->allocation_method & SBC_ALLOCATION_LOUDNESS)
			cap->allocation_method = SBC_ALLOCATION_LOUDNESS;
		else if (cap->allocation_method & SBC_ALLOCATION_SNR)
			cap->allocation_method = SBC_ALLOCATION_SNR;
		else {
			error("No supported allocation: %u", cap->allocation_method);
			goto fail;
		}

		int bitpool = a2dp_sbc_default_bitpool(cap->frequency, cap->channel_mode);
		cap->min_bitpool = MAX(MIN_BITPOOL, cap->min_bitpool);
		cap->max_bitpool = MIN(bitpool, cap->max_bitpool);

	}
	else {
		debug("Endpoint path not supported: %s", path);
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_UNKNOWN_OBJECT, "Not supported");
		goto final;
	}

	GVariantBuilder *caps = g_variant_builder_new(G_VARIANT_TYPE("ay"));
	size_t i;

	for (i = 0; i < size; i++)
		g_variant_builder_add(caps, "y", capabilities[i]);

	g_dbus_method_invocation_return_value(inv, g_variant_new("(ay)", caps));

	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Invalid capabilities");

final:
	g_free(capabilities);
}

static void bluez_endpoint_set_configuration(GDBusMethodInvocation *inv, void *userdata) {

	static GHashTable *profiles = NULL;

	GDBusConnection *conn = g_dbus_method_invocation_get_connection(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	GHashTable *devices = (GHashTable *)userdata;
	struct ba_transport *t;
	struct ba_device *d;
	int profile = -1;
	int codec = -1;

	const char *transport;
	char *device = NULL, *uuid = NULL, *state = NULL;
	uint8_t *config = NULL;
	uint16_t volume = 100;
	size_t size = 0;

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

	GVariantIter *properties;
	GVariant *value = NULL;
	const char *key;

	g_variant_get(params, "(&oa{sv})", &transport, &properties);

	if (transport_lookup(devices, transport) != NULL) {
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

			g_variant_get(value, "o", &device);

		}
		else if (strcmp(key, "UUID") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "s");
				goto fail;
			}

			g_variant_get(value, "s", &uuid);

			const gchar *path = g_dbus_method_invocation_get_object_path(inv);
			gchar *key = g_strdup_printf("%s%s", uuid, path);

			profile = GPOINTER_TO_INT(g_hash_table_lookup(profiles, key));
			g_free(key);

			if (profile == 0) {
				error("UUID %s of transport %s incompatible with endpoint %s", uuid, transport, path);
				goto fail;
			}

		}
		else if (strcmp(key, "Codec") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "y");
				goto fail;
			}

			g_variant_get(value, "y", &codec);

		}
		else if (strcmp(key, "Configuration") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "ay");
				goto fail;
			}

			const guchar *capabilities;

			capabilities = g_variant_get_fixed_array(value, &size, sizeof(uint8_t));
			config = g_memdup(capabilities, size);

			if (codec == A2DP_CODEC_SBC) {

				if (size != sizeof(a2dp_sbc_t)) {
					error("Invalid configuration: %s", "Invalid size");
					goto fail;
				}

				a2dp_sbc_t *cap = (a2dp_sbc_t *)capabilities;

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
					error("Invalid configuration: %s:", "Invalid allocation method");
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

			}

		}
		else if (strcmp(key, "State") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "s");
				goto fail;
			}

			g_variant_get(value, "s", &state);

		}
		else if (strcmp(key, "Delay") == 0) {
		}
		else if (strcmp(key, "Volume") == 0) {

			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16)) {
				error("Invalid argument type for %s: %s != %s", key,
						g_variant_get_type_string(value), "q");
				goto fail;
			}

			g_variant_get(value, "q", &volume);

			/* scale volume from 0 to 100 */
			volume = volume * 100 / 127;

		}

		g_variant_unref(value);
		value = NULL;
	}

	/* If the device is not in our "repository" yet, add it. */
	if ((d = g_hash_table_lookup(devices, device)) == NULL) {

		GVariant *property;
		bdaddr_t addr;
		char name[32];

		g_dbus_devpath_to_bdaddr(device, &addr);
		ba2str(&addr, name);

		if ((property = g_dbus_get_property(conn, "org.bluez", device,
						"org.bluez.Device1", "Name")) != NULL) {
			strncpy(name, g_variant_get_string(property, NULL), sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
			g_variant_unref(property);
		}

		d = device_new(&addr, name);
		g_hash_table_insert(devices, g_strdup(device), d);

	}

	/* Create a new transport with a human-readable name. Since the transport
	 * name can not be obtained from the client, we will use a fall-back one. */
	if ((t = transport_new(conn, g_dbus_method_invocation_get_sender(inv), transport,
					bluetooth_profile_to_string(profile, codec), profile, codec, config, size)) == NULL) {
		error("Cannot create new transport: %s", strerror(errno));
		goto fail;
	}

	transport_set_state_from_string(t, state);
	t->volume = volume;

	g_hash_table_insert(d->transports, g_strdup(transport), t);

	debug("%s configured for device %s", t->name, batostr_(&d->addr));
	g_dbus_method_invocation_return_value(inv, NULL);

	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to set configuration");

final:
	g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
	g_free(device);
	g_free(uuid);
	g_free(config);
	g_free(state);
}

static void bluez_endpoint_clear_configuration(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	GHashTable *devices = (GHashTable *)userdata;
	const char *transport;

	g_variant_get(params, "(&o)", &transport);
	transport_remove(devices, transport);

	g_object_unref(inv);
}

static void bluez_endpoint_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;
	g_object_unref(inv);
}

static void bluez_endpoint_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)params;

	debug("Endpoint method call: %s.%s()", interface, method);

	if (strcmp(method, "SelectConfiguration") == 0)
		bluez_endpoint_select_configuration(invocation, userdata);
	else if (strcmp(method, "SetConfiguration") == 0)
		bluez_endpoint_set_configuration(invocation, userdata);
	else if (strcmp(method, "ClearConfiguration") == 0)
		bluez_endpoint_clear_configuration(invocation, userdata);
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
 * Register A2DP endpoints.
 *
 * @param conn D-Bus connection handler.
 * @param device HCI device name for which endpoints should be registered.
 * @param userdata Data passed to the endpoint handler.
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_register_a2dp(GDBusConnection *conn, const char *device, void *userdata) {

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

#if 0
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
#endif

#if 0
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
#endif

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
#if 0
		{
			BLUETOOTH_UUID_A2DP_SOURCE,
			BLUEZ_ENDPOINT_A2DP_MPEG12_SOURCE,
			A2DP_CODEC_MPEG12,
			&a2dp_mpeg,
			sizeof(a2dp_mpeg),
		},
#endif
#if 0
		{
			BLUETOOTH_UUID_A2DP_SOURCE,
			BLUEZ_ENDPOINT_A2DP_MPEG24_SOURCE,
			A2DP_CODEC_MPEG24,
			&a2dp_aac,
			sizeof(a2dp_aac),
		},
#endif
		{
			BLUETOOTH_UUID_A2DP_SINK,
			BLUEZ_ENDPOINT_A2DP_SBC_SINK,
			A2DP_CODEC_SBC,
			&a2dp_sbc,
			sizeof(a2dp_sbc),
		},
#if 0
		{
			BLUETOOTH_UUID_A2DP_SINK,
			BLUEZ_ENDPOINT_A2DP_MPEG12_SINK,
			A2DP_CODEC_MPEG12,
			&a2dp_mpeg,
			sizeof(a2dp_mpeg),
		},
#endif
#if 0
		{
			BLUETOOTH_UUID_A2DP_SINK,
			BLUEZ_ENDPOINT_A2DP_MPEG24_SINK,
			A2DP_CODEC_MPEG24,
			&a2dp_aac,
			sizeof(a2dp_aac),
		},
#endif
	};

	char *path;
	size_t i;

	path = g_strdup_printf("/org/bluez/%s", device);

	for (i = 0; i < sizeof(endpoints) / sizeof(struct endpoint); i++) {

		GDBusMessage *msg = NULL, *rep = NULL;
		GError *err = NULL;
		guint id;

		debug("Registering endpoint: %s: %s", endpoints[i].uuid, endpoints[i].endpoint);

		if ((id = g_dbus_connection_register_object(conn, endpoints[i].endpoint,
						(GDBusInterfaceInfo *)&bluez_iface_endpoint, &endpoint_vtable,
						userdata, endpoint_free, &err)) == 0)
			goto fail;

		msg = g_dbus_message_new_method_call("org.bluez", path,
				"org.bluez.Media1", "RegisterEndpoint");

		GVariantBuilder *payload = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
		GVariantBuilder *caps = g_variant_builder_new(G_VARIANT_TYPE("ay"));
		size_t ii;

		for (ii = 0; ii < endpoints[i].config_size; ii++)
			g_variant_builder_add(caps, "y", ((uint8_t *)endpoints[i].config)[ii]);

		g_variant_builder_add(payload, "{sv}", "UUID", g_variant_new_string(endpoints[i].uuid));
		g_variant_builder_add(payload, "{sv}", "Codec", g_variant_new_byte(endpoints[i].codec));
		g_variant_builder_add(payload, "{sv}", "Capabilities", g_variant_new("ay", caps));

		g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", endpoints[i].endpoint, payload));

		if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
						G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
			goto fail;

		if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
			g_dbus_message_to_gerror(rep, &err);
			goto fail;
		}

fail:
		if (msg != NULL)
			g_object_unref(msg);
		if (rep != NULL)
			g_object_unref(rep);
		if (err != NULL) {
			warn("Couldn't register endpoint: %s", err->message);
			g_dbus_connection_unregister_object(conn, id);
			g_error_free(err);
		}
	}

	g_free(path);
	return 0;
}

static void bluez_profile_new_connection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_NOT_SUPPORTED, "Not implemented yet");
}

static void bluez_profile_request_disconnection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_NOT_SUPPORTED, "Not implemented yet");
}

static void bluez_profile_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;
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
 * Register Bluetooth Audio Profiles.
 *
 * @param conn D-Bus connection handler.
 * @param device HCI device name for which profile should be registered.
 * @param userdata Data passed to the profile handler.
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_register_hsp(GDBusConnection *conn, const char *device, void *userdata) {
	(void)device;

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	guint id;

	debug("Registering profile: %s: %s", BLUETOOTH_UUID_HSP_AG, BLUEZ_PROFILE_HSP_AG);

	if ((id = g_dbus_connection_register_object(conn, BLUEZ_PROFILE_HSP_AG,
					(GDBusInterfaceInfo *)&bluez_iface_profile, &profile_vtable,
					userdata, profile_free, &err)) == 0)
		goto fail;

	msg = g_dbus_message_new_method_call("org.bluez", "/org/bluez",
			"org.bluez.ProfileManager1", "RegisterProfile");

	g_dbus_message_set_body(msg, g_variant_new("(osa{sv})",
				BLUEZ_PROFILE_HSP_AG, BLUETOOTH_UUID_HSP_AG, NULL));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't register profile: %s", err->message);
		g_dbus_connection_unregister_object(conn, id);
		g_error_free(err);
		return -1;
	}

	return 0;
}

static void bluez_signal_transport_changed(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface;

	GHashTable *devices = (GHashTable *)userdata;
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

	if ((t = transport_lookup(devices, path)) == NULL) {
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

			const char *state;
			g_variant_get(value, "&s", &state);
			transport_set_state_from_string(t, state);

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
 * Subscribe to audio related signals.
 *
 * @param conn D-Bus connection handler.
 * @param device HCI device name for which subscription should be made.
 * @param userdata Data passed to the signal handler.
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_subscribe_signals(GDBusConnection *conn, const char *device, void *userdata) {
	(void)device;

	g_dbus_connection_signal_subscribe(conn, "org.bluez", "org.freedesktop.DBus.Properties",
			"PropertiesChanged", NULL, "org.bluez.MediaTransport1", G_DBUS_SIGNAL_FLAGS_NONE,
			bluez_signal_transport_changed, userdata, NULL);

	return 0;
}
