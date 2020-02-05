/*
 * BlueALSA - bluez.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluez.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "a2dp-codecs.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa.h"
#include "bluealsa-dbus.h"
#include "bluez-a2dp.h"
#include "bluez-iface.h"
#include "hci.h"
#include "sco.h"
#include "utils.h"
#include "shared/defs.h"
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
	/* D-Bus object registration path */
	char path[64];
	/* associated adapter */
	const int hci_dev_id;
	const struct bluez_a2dp_codec *codec;
	const struct ba_transport_type ttype;
	/* determine whether object is registered in BlueZ */
	bool registered;
	/* determine whether object is used */
	bool connected;
};

static GHashTable *dbus_object_data_map = NULL;

/**
 * List of adapters created by BlueZ integration. */
static struct ba_adapter *bluez_adapters[HCI_MAX_DEV] = { NULL };

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
		return ba_transport_set_state(t, BA_TRANSPORT_STATE_IDLE);
	else if (strcmp(state, BLUEZ_TRANSPORT_STATE_PENDING) == 0)
		return ba_transport_set_state(t, BA_TRANSPORT_STATE_PENDING);
	else if (strcmp(state, BLUEZ_TRANSPORT_STATE_ACTIVE) == 0)
		return ba_transport_set_state(t, BA_TRANSPORT_STATE_ACTIVE);

	warn("Invalid state: %s", state);
	return -1;
}

static void bluez_endpoint_select_configuration(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	const struct dbus_object_data *dbus_obj = userdata;
	const struct bluez_a2dp_codec *codec = dbus_obj->codec;

	const void *data;
	void *capabilities;
	size_t size = 0;

	params = g_variant_get_child_value(params, 0);
	data = g_variant_get_fixed_array(params, &size, sizeof(char));
	capabilities = g_memdup(data, size);
	g_variant_unref(params);

	if (size != codec->cfg_size) {
		error("Invalid capabilities size: %zu != %zu", size, codec->cfg_size);
		goto fail;
	}

	switch (codec->id) {
	case A2DP_CODEC_SBC: {

		a2dp_sbc_t *cap = capabilities;
		unsigned int cap_chm = cap->channel_mode;
		unsigned int cap_freq = cap->frequency;

		if ((cap->channel_mode = bluez_a2dp_codec_select_channel_mode(codec, cap_chm)) == 0) {
			error("No supported channel modes: %#x", cap_chm);
			goto fail;
		}

		if (config.sbc_xq) {
			if (cap_chm & SBC_CHANNEL_MODE_DUAL_CHANNEL)
				cap->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
			else
				warn("SBC dual channel mode not supported: %#x", cap_chm);
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
		if (config.sbc_xq)
			bitpool = 38;

		cap->min_bitpool = MAX(SBC_MIN_BITPOOL, cap->min_bitpool);
		cap->max_bitpool = MIN(bitpool, cap->max_bitpool);

		break;
	}

#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12: {

		a2dp_mpeg_t *cap = capabilities;
		unsigned int cap_chm = cap->channel_mode;
		unsigned int cap_freq = cap->frequency;

		if (cap->layer & MPEG_LAYER_MP3)
			cap->layer = MPEG_LAYER_MP3;
		else {
			error("No supported layer: %#x", cap->layer);
			goto fail;
		}

		if ((cap->channel_mode = bluez_a2dp_codec_select_channel_mode(codec, cap_chm)) == 0) {
			error("No supported channel modes: %#x", cap_chm);
			goto fail;
		}

		if ((cap->frequency = bluez_a2dp_codec_select_sampling_freq(codec, cap_freq)) == 0) {
			error("No supported sampling frequencies: %#x", cap_freq);
			goto fail;
		}

		/* do not waste bits for CRC protection */
		cap->crc = 0;
		/* do not use MPF-2 */
		cap->mpf = 0;

		break;
	}
#endif

#if ENABLE_AAC
	case A2DP_CODEC_MPEG24: {

		a2dp_aac_t *cap = capabilities;
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

		a2dp_aptx_t *cap = capabilities;
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

#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD: {

		a2dp_aptx_hd_t *cap = capabilities;
		unsigned int cap_chm = cap->aptx.channel_mode;
		unsigned int cap_freq = cap->aptx.frequency;

		if ((cap->aptx.channel_mode = bluez_a2dp_codec_select_channel_mode(codec, cap_chm)) == 0) {
			error("No supported channel modes: %#x", cap_chm);
			goto fail;
		}

		if ((cap->aptx.frequency = bluez_a2dp_codec_select_sampling_freq(codec, cap_freq)) == 0) {
			error("No supported sampling frequencies: %#x", cap_freq);
			goto fail;
		}

		break;
	}
#endif

#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC: {

		a2dp_ldac_t *cap = capabilities;
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
		debug("Endpoint path not supported: %s", dbus_obj->path);
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_UNKNOWN_OBJECT, "Not supported");
		goto final;
	}

	GVariantBuilder caps;
	size_t i;

	g_variant_builder_init(&caps, G_VARIANT_TYPE("ay"));
	for (i = 0; i < size; i++)
		g_variant_builder_add(&caps, "y", ((char *)capabilities)[i]);

	g_dbus_method_invocation_return_value(inv, g_variant_new("(ay)", &caps));
	g_variant_builder_clear(&caps);

	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Invalid capabilities");

final:
	g_free(capabilities);
}

static void bluez_endpoint_set_configuration(GDBusMethodInvocation *inv, void *userdata) {

	const char *sender = g_dbus_method_invocation_get_sender(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct dbus_object_data *dbus_obj = userdata;
	const struct bluez_a2dp_codec *codec = dbus_obj->codec;
	const uint16_t codec_id = codec->id;

	struct ba_adapter *a = NULL;
	struct ba_transport *t = NULL;
	struct ba_device *d = NULL;

	char *state = NULL;
	char *device_path = NULL;
	void *capabilities = NULL;
	uint16_t volume = 127;
	uint16_t delay = 150;
	size_t size = 0;

	const char *transport_path;
	GVariantIter *properties;
	GVariant *value = NULL;
	const char *property;

	g_variant_get(params, "(&oa{sv})", &transport_path, &properties);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {

		if (strcmp(property, "Device") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_OBJECT_PATH, property)) {
			device_path = g_variant_dup_string(value, NULL);
		}
		else if (strcmp(property, "UUID") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
		}
		else if (strcmp(property, "Codec") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTE, property)) {

			if ((codec_id & 0xFF) != g_variant_get_byte(value)) {
				error("Invalid configuration: %s", "Codec mismatch");
				goto fail;
			}

		}
		else if (strcmp(property, "Configuration") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTESTRING, property)) {

			const void *data = g_variant_get_fixed_array(value, &size, sizeof(char));
			unsigned int cap_chm = 0;
			unsigned int cap_freq = 0;

			capabilities = g_memdup(data, size);

			if (size != codec->cfg_size) {
				error("Invalid configuration: %s", "Invalid size");
				goto fail;
			}

			switch (codec_id) {
			case A2DP_CODEC_SBC: {

				const a2dp_sbc_t *cap = capabilities;
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
				a2dp_mpeg_t *cap = capabilities;
				cap_chm = cap->channel_mode;
				cap_freq = cap->frequency;
				break;
			}
#endif

#if ENABLE_AAC
			case A2DP_CODEC_MPEG24: {

				const a2dp_aac_t *cap = capabilities;
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
				a2dp_aptx_t *cap = capabilities;
				cap_chm = cap->channel_mode;
				cap_freq = cap->frequency;
				break;
			}
#endif

#if ENABLE_APTX_HD
			case A2DP_CODEC_VENDOR_APTX_HD: {
				a2dp_aptx_hd_t *cap = capabilities;
				cap_chm = cap->aptx.channel_mode;
				cap_freq = cap->aptx.frequency;
				break;
			}
#endif

#if ENABLE_LDAC
			case A2DP_CODEC_VENDOR_LDAC: {
				a2dp_ldac_t *cap = capabilities;
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
		else if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
			state = g_variant_dup_string(value, NULL);
		}
		else if (strcmp(property, "Delay") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			delay = g_variant_get_uint16(value);
		}
		else if (strcmp(property, "Volume") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			/* received volume is in range [0, 127] */
			volume = g_variant_get_uint16(value);
		}

		g_variant_unref(value);
		value = NULL;
	}

	if ((a = ba_adapter_lookup(dbus_obj->hci_dev_id)) == NULL) {
		error("Couldn't lookup adapter: hci%d: %s", dbus_obj->hci_dev_id, strerror(errno));
		goto fail;
	}

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL &&
			(d = ba_device_new(a, &addr)) == NULL) {
		error("Couldn't create new device: %s", device_path);
		goto fail;
	}

	if ((t = ba_transport_lookup(d, transport_path)) != NULL) {
		error("Transport already configured: %s", transport_path);
		goto fail;
	}

	if ((t = ba_transport_new_a2dp(d, dbus_obj->ttype,
					sender, transport_path, capabilities, size)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->a2dp.pcm.volume[0].level = volume;
	t->a2dp.pcm.volume[1].level = volume;
	t->a2dp.delay = delay;

	debug("%s configured for device %s",
			ba_transport_type_to_string(t->type),
			batostr_(&d->addr));
	debug("Configuration: channels: %u, sampling: %u",
			t->a2dp.pcm.channels, t->a2dp.pcm.sampling);

	bluez_a2dp_set_transport_state(t, state);
	dbus_obj->connected = true;

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to set configuration");

final:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
	g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
	g_free(device_path);
	g_free(capabilities);
	g_free(state);
}

static void bluez_endpoint_clear_configuration(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct dbus_object_data *dbus_obj = userdata;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	debug("Disconnecting media endpoint: %s", dbus_obj->path);
	dbus_obj->connected = false;

	const char *transport_path;
	g_variant_get(params, "(&o)", &transport_path);

	if ((a = ba_adapter_lookup(dbus_obj->hci_dev_id)) == NULL)
		goto fail;

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(transport_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL)
		goto fail;

	if ((t = ba_transport_lookup(d, transport_path)) != NULL)
		ba_transport_destroy(t);

fail:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	g_object_unref(inv);
}

static void bluez_endpoint_release(GDBusMethodInvocation *inv, void *userdata) {

	struct dbus_object_data *dbus_obj = userdata;

	debug("Releasing media endpoint: %s", dbus_obj->path);
	dbus_obj->connected = false;
	dbus_obj->registered = false;

	g_object_unref(inv);
}

static void bluez_register_a2dp_all(struct ba_adapter *adapter);

static void bluez_endpoint_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	debug("Called: %s.%s()", interface, method);
	(void)conn;
	(void)sender;
	(void)path;
	(void)params;

	struct dbus_object_data *dbus_obj = userdata;
	struct ba_adapter *a;

	if (strcmp(method, "SelectConfiguration") == 0)
		bluez_endpoint_select_configuration(invocation, userdata);
	else if (strcmp(method, "SetConfiguration") == 0) {
		bluez_endpoint_set_configuration(invocation, userdata);
		if ((a = ba_adapter_lookup(dbus_obj->hci_dev_id)) != NULL) {
			bluez_register_a2dp_all(a);
			ba_adapter_unref(a);
		}
	}
	else if (strcmp(method, "ClearConfiguration") == 0)
		bluez_endpoint_clear_configuration(invocation, userdata);
	else if (strcmp(method, "Release") == 0)
		bluez_endpoint_release(invocation, userdata);

}

/**
 * Register media endpoint object in D-Bus. */
static struct dbus_object_data *bluez_create_media_endpoint_object(
		const struct ba_adapter *adapter,
		const struct ba_transport_type ttype,
		const struct bluez_a2dp_codec *codec,
		const char *path,
		GError **error) {

	static GDBusInterfaceVTable vtable = {
		.method_call = bluez_endpoint_method_call,
	};

	struct dbus_object_data *dbus_obj;
	struct dbus_object_data dbus_object = {
		.hci_dev_id = adapter->hci.dev_id,
		.codec = codec,
		.ttype = ttype,
	};

	debug("Creating media endpoint object: %s", path);

	strncpy(dbus_object.path, path, sizeof(dbus_object.path));
	dbus_obj = g_memdup(&dbus_object, sizeof(dbus_object));

	if ((dbus_obj->id = g_dbus_connection_register_object(config.dbus,
					dbus_object.path, (GDBusInterfaceInfo *)&bluez_iface_endpoint,
					&vtable, dbus_obj, NULL, error)) == 0) {
		g_free(dbus_obj);
		return NULL;
	}

	g_hash_table_insert(dbus_object_data_map, dbus_obj->path, dbus_obj);
	return dbus_obj;
}

/**
 * Register media endpoint in BlueZ. */
static int bluez_register_media_endpoint(
		const struct ba_adapter *adapter,
		const struct dbus_object_data *dbus_obj,
		const char *uuid,
		GError **error) {

	const struct bluez_a2dp_codec *codec = dbus_obj->codec;
	GDBusMessage *msg = NULL, *rep = NULL;
	int ret = 0;
	size_t i;

	debug("Registering media endpoint: %s", dbus_obj->path);

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

	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", dbus_obj->path, &properties));
	g_variant_builder_clear(&properties);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return ret;
}

/**
 * Register A2DP endpoint. */
static void bluez_register_a2dp(
		const struct ba_adapter *adapter,
		const struct bluez_a2dp_codec *codec,
		const char *uuid) {

	struct ba_transport_type ttype = {
		.profile = codec->dir == BLUEZ_A2DP_SOURCE ?
			BA_TRANSPORT_PROFILE_A2DP_SOURCE : BA_TRANSPORT_PROFILE_A2DP_SINK,
		.codec = codec->id,
	};

	int registered = 0;
	int connected = 0;

	for (;;) {

		struct dbus_object_data *dbus_obj;
		GError *err = NULL;

		char path[sizeof(dbus_obj->path)];
		snprintf(path, sizeof(path), "/org/bluez/%s%s/%d", adapter->hci.name,
				g_dbus_transport_type_to_bluez_object_path(ttype), ++registered);

		dbus_obj = g_hash_table_lookup(dbus_object_data_map, path);

		/* End the registration loop if all previously created media endpoints are
		 * registered in BlueZ and we've got at least N not connected endpoints. */
		if (dbus_obj == NULL && registered > connected + 2)
			break;

		if (dbus_obj == NULL && (dbus_obj = bluez_create_media_endpoint_object(adapter,
						ttype, codec, path, &err)) == NULL)
			goto fail;

		if (!dbus_obj->registered) {
			if (bluez_register_media_endpoint(adapter, dbus_obj, uuid, &err) == -1)
				goto fail;
			dbus_obj->registered = true;
		}

		if (dbus_obj->connected)
			connected++;

		continue;

fail:
		if (err != NULL) {
			warn("Couldn't register media endpoint: %s", err->message);
			g_error_free(err);
		}
	}

}

/**
 * Register A2DP endpoints. */
static void bluez_register_a2dp_all(struct ba_adapter *adapter) {

	const struct bluez_a2dp_codec **cc = config.a2dp.codecs;

	while (*cc != NULL) {
		const struct bluez_a2dp_codec *c = *cc++;
		switch (c->dir) {
		case BLUEZ_A2DP_SOURCE:
			if (config.enable.a2dp_source)
				bluez_register_a2dp(adapter, c, BLUETOOTH_UUID_A2DP_SOURCE);
			break;
		case BLUEZ_A2DP_SINK:
			if (config.enable.a2dp_sink)
				bluez_register_a2dp(adapter, c, BLUETOOTH_UUID_A2DP_SINK);
			break;
		}
	}

}

static void bluez_profile_new_connection(GDBusMethodInvocation *inv, void *userdata) {

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	const char *sender = g_dbus_method_invocation_get_sender(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct dbus_object_data *dbus_obj = userdata;

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
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL) {
		error("Couldn't lookup adapter: hci%d: %s", hci_dev_id, strerror(errno));
		goto fail;
	}

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL &&
			(d = ba_device_new(a, &addr)) == NULL) {
		error("Couldn't create new device: %s", strerror(errno));
		goto fail;
	}

	if ((t = ba_transport_new_rfcomm(d, dbus_obj->ttype,
					sender, device_path)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->bt_fd = fd;

	if (sco_setup_connection_dispatcher(a) == -1) {
		error("Couldn't setup SCO connection dispatcher: %s", strerror(errno));
		goto fail;
	}

	debug("%s configured for device %s",
			ba_transport_type_to_string(t->type),
			batostr_(&d->addr));

	ba_transport_set_state(t, BA_TRANSPORT_STATE_ACTIVE);
	ba_transport_set_state(t->rfcomm.sco, BA_TRANSPORT_STATE_ACTIVE);
	dbus_obj->connected = true;

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to connect profile");
	if (fd != -1)
		close(fd);

final:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
	g_variant_iter_free(properties);
	if (err != NULL)
		g_error_free(err);
}

static void bluez_profile_request_disconnection(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct dbus_object_data *dbus_obj = userdata;

	debug("Disconnecting hands-free profile: %s", dbus_obj->path);
	dbus_obj->connected = false;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *device_path;
	g_variant_get(params, "(&o)", &device_path);

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(device_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL)
		goto fail;

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL)
		goto fail;

	if ((t = ba_transport_lookup(d, device_path)) != NULL)
		ba_transport_destroy(t);

fail:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	g_object_unref(inv);
}

static void bluez_profile_release(GDBusMethodInvocation *inv, void *userdata) {

	struct dbus_object_data *dbus_obj = userdata;

	debug("Releasing hands-free profile: %s", dbus_obj->path);
	dbus_obj->connected = false;
	dbus_obj->registered = false;

	g_object_unref(inv);
}

static void bluez_profile_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	debug("Called: %s.%s()", interface, method);
	(void)conn;
	(void)sender;
	(void)path;
	(void)params;

	if (strcmp(method, "NewConnection") == 0)
		bluez_profile_new_connection(invocation, userdata);
	else if (strcmp(method, "RequestDisconnection") == 0)
		bluez_profile_request_disconnection(invocation, userdata);
	else if (strcmp(method, "Release") == 0)
		bluez_profile_release(invocation, userdata);

}

/**
 * Register hands-free profile object in D-Bus. */
static struct dbus_object_data *bluez_create_profile_object(
		const struct ba_transport_type ttype,
		const char *path,
		GError **error) {

	static GDBusInterfaceVTable vtable = {
		.method_call = bluez_profile_method_call,
	};

	struct dbus_object_data *dbus_obj;
	struct dbus_object_data dbus_object = {
		.hci_dev_id = -1,
		.ttype = ttype,
	};

	debug("Creating hands-free profile object: %s", path);

	strncpy(dbus_object.path, path, sizeof(dbus_object.path));
	dbus_obj = g_memdup(&dbus_object, sizeof(dbus_object));

	if ((dbus_obj->id = g_dbus_connection_register_object(config.dbus,
					dbus_object.path, (GDBusInterfaceInfo *)&bluez_iface_profile,
					&vtable, dbus_obj, NULL, error)) == 0) {
		g_free(dbus_obj);
		return NULL;
	}

	g_hash_table_insert(dbus_object_data_map, dbus_obj->path, dbus_obj);
	return dbus_obj;
}

/**
 * Register hands-free profile in BlueZ. */
static int bluez_register_profile(
		const struct dbus_object_data *dbus_obj,
		const char *uuid,
		uint16_t version,
		uint16_t features,
		GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	int ret = 0;

	debug("Registering hands-free profile: %s", dbus_obj->path);

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_IFACE_PROFILE_MANAGER, "RegisterProfile");

	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&options, "{sv}", "Name",
			g_variant_new_string(ba_transport_type_to_string(dbus_obj->ttype)));
	g_variant_builder_add(&options, "{sv}", "Channel", g_variant_new_uint16(0));
	if (version)
		g_variant_builder_add(&options, "{sv}", "Version", g_variant_new_uint16(version));
	if (features)
		g_variant_builder_add(&options, "{sv}", "Features", g_variant_new_uint16(features));

	g_dbus_message_set_body(msg, g_variant_new("(osa{sv})", dbus_obj->path, uuid, &options));
	g_variant_builder_clear(&options);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return ret;
}

/**
 * Register Bluetooth Hands-Free Audio Profile. */
static void bluez_register_hfp(
		const char *uuid,
		uint32_t profile,
		uint16_t version,
		uint16_t features) {

	struct ba_transport_type ttype = {
		.profile = profile,
	};

	struct dbus_object_data *dbus_obj;
	GError *err = NULL;

	char path[sizeof(dbus_obj->path)];
	snprintf(path, sizeof(path), "/org/bluez%s",
			g_dbus_transport_type_to_bluez_object_path(ttype));

	if ((dbus_obj = g_hash_table_lookup(dbus_object_data_map, path)) == NULL &&
			(dbus_obj = bluez_create_profile_object(ttype, path, &err)) == NULL)
		goto fail;

	if (!dbus_obj->registered) {
		if (bluez_register_profile(dbus_obj, uuid, version, features, &err) == -1)
			goto fail;
		dbus_obj->registered = true;
	}

	return;

fail:
	if (err != NULL) {
		warn("Couldn't register hands-free profile: %s", err->message);
		g_error_free(err);
	}
}

/**
 * Register Bluetooth Hands-Free Audio Profiles.
 *
 * This function also registers deprecated HSP profile. Profiles registration
 * is controlled by the global configuration structure - if none is enabled,
 * this function will do nothing. */
static void bluez_register_hfp_all(void) {
	if (config.enable.hsp_hs)
		bluez_register_hfp(BLUETOOTH_UUID_HSP_HS, BA_TRANSPORT_PROFILE_HSP_HS,
				0x0102 /* HSP 1.2 */, 0x0);
	if (config.enable.hsp_ag)
		bluez_register_hfp(BLUETOOTH_UUID_HSP_AG, BA_TRANSPORT_PROFILE_HSP_AG,
				0x0102 /* HSP 1.2 */, 0x0);
	if (config.enable.hfp_hf)
		bluez_register_hfp(BLUETOOTH_UUID_HFP_HF, BA_TRANSPORT_PROFILE_HFP_HF,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_hf);
	if (config.enable.hfp_ag)
		bluez_register_hfp(BLUETOOTH_UUID_HFP_AG, BA_TRANSPORT_PROFILE_HFP_AG,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_ag);
}

/**
 * Register to the BlueZ service. */
void bluez_register(void) {

	if (dbus_object_data_map == NULL)
		dbus_object_data_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	GError *err = NULL;
	GVariantIter *objects = NULL;
	if ((objects = g_dbus_get_managed_objects(config.dbus, BLUEZ_SERVICE, "/", &err)) == NULL) {
		warn("Couldn't get managed objects: %s", err->message);
		g_error_free(err);
		return;
	}

	bool adapters[HCI_MAX_DEV] = { 0 };

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
	struct ba_adapter *a;
	for (i = 0; i < ARRAYSIZE(adapters); i++)
		if (adapters[i] &&
				(a = ba_adapter_new(i)) != NULL) {
			bluez_adapters[a->hci.dev_id] = a;
			bluez_register_a2dp_all(a);
		}

	/* HFP has to be registered globally */
	bluez_register_hfp_all();

}

static void bluez_signal_interfaces_added(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface_, signal);
	(void)conn;
	(void)sender;
	(void)path;
	(void)userdata;

	GVariantIter *interfaces;
	GVariantIter *properties;
	GVariant *value;
	const char *object_path;
	const char *interface;
	const char *property;
	int hci_dev_id = -1;

	g_variant_get(params, "(&oa{sa{sv}})", &object_path, &interfaces);
	while (g_variant_iter_next(interfaces, "{&sa{sv}}", &interface, &properties)) {
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0)
			while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
				if (strcmp(property, "Address") == 0 &&
						bluez_match_dbus_adapter(object_path, g_variant_get_string(value, NULL)))
					hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);
				g_variant_unref(value);
			}
		g_variant_iter_free(properties);
	}
	g_variant_iter_free(interfaces);

	struct ba_adapter *a;
	if (hci_dev_id != -1 &&
			(a = ba_adapter_new(hci_dev_id)) != NULL) {
		bluez_adapters[a->hci.dev_id] = a;
		bluez_register_a2dp_all(a);
	}

	/* HFP has to be registered globally */
	if (strcmp(object_path, "/org/bluez") == 0)
		bluez_register_hfp_all();

}

static void bluez_signal_interfaces_removed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface_, signal);
	(void)sender;
	(void)path;
	(void)userdata;

	GVariantIter *interfaces;
	const char *object_path;
	const char *interface;

	g_variant_get(params, "(&oas)", &object_path, &interfaces);
	while (g_variant_iter_next(interfaces, "&s", &interface))
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0) {

			int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);

			GHashTableIter iter;
			struct dbus_object_data *dbus_obj;
			g_hash_table_iter_init(&iter, dbus_object_data_map);
			while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj))
				if (dbus_obj->hci_dev_id == hci_dev_id) {
					g_dbus_connection_unregister_object(conn, dbus_obj->id);
					g_hash_table_iter_remove(&iter);
				}

			struct ba_adapter *a;
			if ((a = ba_adapter_lookup(hci_dev_id)) != NULL) {
				ba_adapter_unref(bluez_adapters[a->hci.dev_id]);
				bluez_adapters[a->hci.dev_id] = NULL;
				ba_adapter_destroy(a);
			}

		}
	g_variant_iter_free(interfaces);

}

static void bluez_signal_transport_changed(GDBusConnection *conn, const char *sender,
		const char *transport_path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface_, signal);
	(void)conn;
	(void)sender;
	(void)userdata;

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

		if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
			bluez_a2dp_set_transport_state(t, g_variant_get_string(value, NULL));
		}
		else if (strcmp(property, "Delay") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			t->a2dp.delay = g_variant_get_uint16(value);
			bluealsa_dbus_pcm_update(&t->a2dp.pcm, BA_DBUS_PCM_UPDATE_DELAY);
		}
		else if (strcmp(property, "Volume") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			/* received volume is in range [0, 127] */
			const uint16_t volume = g_variant_get_uint16(value);
			t->a2dp.pcm.volume[0].level = t->a2dp.pcm.volume[1].level = volume;
			bluealsa_dbus_pcm_update(&t->a2dp.pcm, BA_DBUS_PCM_UPDATE_VOLUME);
		}

		g_variant_unref(value);
	}
	g_variant_iter_free(properties);

final:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
}

/**
 * Monitor BlueZ service availability.
 *
 * When BlueZ is properly shutdown, we are notified about adapter removal via
 * the InterfacesRemoved signal. Here, we get the opportunity to perform some
 * cleanup if BlueZ service was killed. */
static void bluez_signal_name_owner_changed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)signal;
	(void)userdata;

	const char *name;
	const char *owner_old;
	const char *owner_new;

	g_variant_get(params, "(&s&s&s)", &name, &owner_old, &owner_new);
	if (owner_old != NULL && owner_old[0] != '\0') {

		GHashTableIter iter;
		struct dbus_object_data *dbus_obj;
		g_hash_table_iter_init(&iter, dbus_object_data_map);
		while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj)) {
			g_dbus_connection_unregister_object(conn, dbus_obj->id);
			g_hash_table_iter_remove(&iter);
		}

		size_t i;
		for (i = 0; i < ARRAYSIZE(bluez_adapters); i++)
			if (bluez_adapters[i] != NULL) {
				ba_adapter_destroy(bluez_adapters[i]);
				bluez_adapters[i] = NULL;
			}

	}

}

/**
 * Subscribe to BlueZ signals.
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
			"org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, BLUEZ_IFACE_MEDIA_TRANSPORT,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_transport_changed, NULL, NULL);

	g_dbus_connection_signal_subscribe(config.dbus, "org.freedesktop.DBus",
			"org.freedesktop.DBus", "NameOwnerChanged", NULL, BLUEZ_SERVICE,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_name_owner_changed, NULL, NULL);

	return 0;
}
