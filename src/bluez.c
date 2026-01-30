/*
 * BlueALSA - bluez.c
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "bluez.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "a2dp.h"
#include "asha.h"
#include "ba-adapter.h"
#include "ba-config.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#if ENABLE_MIDI
# include "bluez-midi.h"
#endif
#include "dbus.h"
#include "error.h"
#include "hci.h"
#include "sco.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/bluetooth.h"
#include "shared/bluetooth-asha.h"
#include "shared/defs.h"
#include "shared/log.h"

/* Compatibility patch for glib < 2.68. */
#if !GLIB_CHECK_VERSION(2, 68, 0)
# define g_memdup2 g_memdup
#endif

/**
 * Data associated with registered D-Bus object. */
struct bluez_dbus_object_data {
	unsigned int index;
	/* D-Bus object registration path */
	char path[64];
	/* exported interface skeleton */
	GDBusInterfaceSkeleton *ifs;
	/* associated adapter */
	int hci_dev_id;
	/* device associated with endpoint object (if connected) */
	const struct ba_device *device;
	/* registered profile */
	enum ba_transport_profile profile;
	/* media endpoint SEP */
	const struct a2dp_sep *sep;
	/* determine whether object is registered in BlueZ */
	bool registered;
	/* determine whether object is used */
	bool connected;
};

/**
 * BlueALSA copy of BlueZ adapter data. */
struct bluez_adapter {
	/* reference to the adapter structure */
	struct ba_adapter *adapter;
	/* manager for media endpoint objects */
	GDBusObjectManagerServer *manager_media_application;
	/* manager for battery provider objects */
	GDBusObjectManagerServer *manager_battery_provider;
#if ENABLE_MIDI
	/* Bluetooth LE MIDI based on BlueZ GATT application. */
	BluetoothMIDI * midi;
#endif
	/* array of SEP configs per connected devices */
	GHashTable *device_sep_configs_map;
};

static pthread_mutex_t bluez_mutex = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *dbus_object_data_map = NULL;
static struct bluez_adapter bluez_adapters[HCI_MAX_DEV] = { 0 };
char bluez_dbus_unique_name[32] = "";

static struct bluez_adapter * bluez_adapter_new(struct ba_adapter * a) {
	struct bluez_adapter * b_adapter = &bluez_adapters[a->hci.dev_id];
	b_adapter->device_sep_configs_map = g_hash_table_new_full(
			g_bdaddr_hash, g_bdaddr_equal, g_free, (GDestroyNotify)g_array_unref);
	b_adapter->adapter = a;
	return b_adapter;
}

static void register_battery_provider_finish(
		GObject * source,
		GAsyncResult * result,
		G_GNUC_UNUSED void * userdata) {

	g_autoptr(GDBusMessage) rep;
	g_autoptr(GError) err = NULL;
	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) == NULL ||
			g_dbus_message_to_gerror(rep, &err)) {
		if (err->code == G_DBUS_ERROR_UNKNOWN_METHOD) {
			/* Suppress warning message in case when BlueZ has no battery provider
			 * support enabled, because it's not a mandatory feature. */
			debug("BlueZ battery provider support not available");
			g_error_free(g_steal_pointer(&err));
		}
	}

	if (err != NULL) {
		error("Couldn't register battery provider: %s", err->message);
	}

}

/**
 * Register battery provider in BlueZ.
 *
 * This function requires Battery Provider Manager interface to be available
 * on the given adapter. */
static void bluez_adapter_register_battery_provider(
		struct bluez_adapter * b_adapter) {
	if (config.profile.hfp_ag || config.profile.hfp_hf ||
			config.profile.hsp_ag || config.profile.hsp_hs) {

		char path[64];
		struct ba_adapter * a = b_adapter->adapter;
		snprintf(path, sizeof(path), "/org/bluez/%s/battery", a->hci.name);

		GDBusObjectManagerServer * manager = g_dbus_object_manager_server_new(path);
		g_dbus_object_manager_server_set_connection(manager, config.dbus);
		b_adapter->manager_battery_provider = manager;

		g_autoptr(GDBusMessage) msg = g_dbus_message_new_method_call(BLUEZ_SERVICE,
				a->bluez_dbus_path, BLUEZ_IFACE_BATTERY_PROVIDER_MANAGER, "RegisterBatteryProvider");
		g_dbus_message_set_body(msg, g_variant_new("(o)", path));

		debug("Registering battery provider: %s", path);
		g_dbus_connection_send_message_with_reply(config.dbus, msg,
				G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
				register_battery_provider_finish, NULL);

	}
}

static void register_media_application_finish(
		GObject * source,
		GAsyncResult * result,
		G_GNUC_UNUSED void * userdata) {

	g_autoptr(GDBusMessage) rep;
	g_autoptr(GError) err = NULL;
	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	if (err != NULL) {
		error("Couldn't register media application: %s", err->message);
	}

}

static void bluez_adapter_export_a2dp_all(struct bluez_adapter * b_adapter);

/**
 * Register media application and A2DP endpoints in BlueZ.
 *
 * This function requires Media Manager interface to be available
 * on the given adapter. */
static void bluez_adapter_register_media_application(
		struct bluez_adapter * b_adapter) {
	if (config.profile.a2dp_source || config.profile.a2dp_sink) {

		char path[64];
		struct ba_adapter * a = b_adapter->adapter;
		snprintf(path, sizeof(path), "/org/bluez/%s", a->hci.name);

		GDBusObjectManagerServer * manager = g_dbus_object_manager_server_new(path);
		g_dbus_object_manager_server_set_connection(manager, config.dbus);
		b_adapter->manager_media_application = manager;

		/* Export A2DP endpoints before registering media application. */
		bluez_adapter_export_a2dp_all(b_adapter);

		g_autoptr(GDBusMessage) msg = g_dbus_message_new_method_call(BLUEZ_SERVICE,
				a->bluez_dbus_path, BLUEZ_IFACE_MEDIA, "RegisterApplication");
		g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", path, NULL));

		debug("Registering media application: %s", path);
		g_dbus_connection_send_message_with_reply(config.dbus, msg,
				G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
				register_media_application_finish, a);

	}
}

/**
 * Register GATT applications in BlueZ.
 *
 * This function requires GATT Manager and LE Advertising Manager interfaces
 * to be available on the given adapter. */
static void bluez_adapter_register_gatt_applications(
		G_GNUC_UNUSED struct bluez_adapter * b_adapter) {
#if ENABLE_MIDI
	if (config.profile.midi) {
		char path[64];
		struct ba_adapter * a = b_adapter->adapter;
		snprintf(path, sizeof(path), "/org/bluez/%s/MIDI", a->hci.name);
		b_adapter->midi = bluetooth_midi_new(a, path);
	}
#endif
}

static void bluez_adapter_free(
		struct bluez_adapter * b_adapter) {
	if (b_adapter->adapter == NULL)
		return;
#if ENABLE_MIDI
	g_clear_object(&b_adapter->midi);
#endif
	ba_adapter_destroy(g_steal_pointer(&b_adapter->adapter));
	g_clear_object(&b_adapter->manager_media_application);
	g_clear_object(&b_adapter->manager_battery_provider);
	g_hash_table_unref(g_steal_pointer(&b_adapter->device_sep_configs_map));
}

/**
 * Get Stream End-Point configurations associated with the given device. */
static GArray * bluez_adapter_get_device_sep_configs(
		struct bluez_adapter * b_adapter,
		const bdaddr_t * addr) {
	GArray * sep_cfgs;
	if ((sep_cfgs = g_hash_table_lookup(b_adapter->device_sep_configs_map, addr)) != NULL)
		return sep_cfgs;
	sep_cfgs = g_array_new(FALSE, FALSE, sizeof(struct a2dp_sep_config));
	g_hash_table_insert(b_adapter->device_sep_configs_map, g_memdup2(addr, sizeof(*addr)), sep_cfgs);
	return sep_cfgs;
}

/**
 * Associate/disassociate device with registered media endpoint object. */
static void bluez_dbus_object_data_device_set(
		struct bluez_dbus_object_data *obj,
		struct ba_device *d) {

	obj->device = d;

	GVariant *changed = NULL;
	GVariant *invalidated = NULL;

	if (d != NULL) {
		GVariantBuilder props;
		g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&props, "{sv}", "Device",
				g_variant_new_object_path(d->bluez_dbus_path));
		changed = g_variant_builder_end(&props);
	}
	else {
		const char *props[] = { "Device" };
		invalidated = g_variant_new_strv(props, 1);
	}

	g_dbus_connection_emit_properties_changed(config.dbus, obj->path,
			BLUEZ_IFACE_MEDIA_ENDPOINT, changed, invalidated, NULL);

}

static void bluez_dbus_object_data_free(
		struct bluez_dbus_object_data *obj) {
	if (obj->ifs != NULL) {
		g_dbus_interface_skeleton_unexport(obj->ifs);
		g_object_unref(obj->ifs);
	}
	free(obj);
}

/**
 * Check whether D-Bus adapter matches our configuration. */
static bool bluez_match_dbus_adapter(
		const char *adapter_path,
		const char *adapter_address) {

	/* if configuration is empty, match everything */
	if (config.hci_filter->len == 0)
		return true;

	/* get the last component of the path */
	if ((adapter_path = strrchr(adapter_path, '/')) == NULL)
		return false;

	adapter_path++;

	for (size_t i = 0; i < config.hci_filter->len; i++)
		if (strcasecmp(adapter_path, g_array_index(config.hci_filter, char *, i)) == 0 ||
				strcasecmp(adapter_address, g_array_index(config.hci_filter, char *, i)) == 0)
			return true;

	return false;
}

static const char *bluez_get_media_endpoint_object_path(
		const struct ba_adapter *adapter,
		const struct a2dp_sep *sep,
		unsigned int index) {

	static char path[64];

	const char *tmp;
	char codec_name[16] = "";
	if ((tmp = a2dp_codecs_codec_id_to_string(sep->config.codec_id)) == NULL)
		snprintf(codec_name, sizeof(codec_name), "%08x", sep->config.codec_id);
	else {
		for (size_t i = 0, j = 0; tmp[i] != '\0' && j < sizeof(codec_name); i++)
			if (isupper(tmp[i]) || islower(tmp[i]) || isdigit(tmp[i]))
				codec_name[j++] = tmp[i];
	}

	snprintf(path, sizeof(path), "/org/bluez/%s/A2DP/%s/%s/%u", adapter->hci.name,
			codec_name, sep->config.type == A2DP_SOURCE ? "source" : "sink", index);

	return path;
}

static uint8_t bluez_get_media_endpoint_codec(
		const struct a2dp_sep *sep) {
	if (sep->config.codec_id < A2DP_CODEC_VENDOR)
		return sep->config.codec_id;
	return A2DP_CODEC_VENDOR;
}

static const char *bluez_get_profile_object_path(
		enum ba_transport_profile profile) {
	switch (profile) {
	case BA_TRANSPORT_PROFILE_HFP_HF:
		return "/org/bluez/HFP/HandsFree";
	case BA_TRANSPORT_PROFILE_HFP_AG:
		return "/org/bluez/HFP/AudioGateway";
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "/org/bluez/HSP/Headset";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "/org/bluez/HSP/AudioGateway";
	default:
		g_assert_not_reached();
		return "/";
	}
}

/**
 * Get media transport state from BlueZ state string. */
static enum bluez_media_transport_state media_transport_state_from_string(
		const char *state) {
	if (strcmp(state, "idle") == 0)
		return BLUEZ_MEDIA_TRANSPORT_STATE_IDLE;
	if (strcmp(state, "pending") == 0)
		return BLUEZ_MEDIA_TRANSPORT_STATE_PENDING;
	if (strcmp(state, "broadcasting") == 0)
		return BLUEZ_MEDIA_TRANSPORT_STATE_BROADCASTING;
	if (strcmp(state, "active") == 0)
		return BLUEZ_MEDIA_TRANSPORT_STATE_ACTIVE;
	warn("Invalid media transport state: %s", state);
	return BLUEZ_MEDIA_TRANSPORT_STATE_IDLE;
}

static const char * media_endpoint_error_from_error_code(error_code_t err) {
	switch (err) {
	case ERROR_CODE_A2DP_INVALID_CHANNELS:
		return BLUEZ_ERROR_A2DP_INVALID_CHANNELS;
	case ERROR_CODE_A2DP_INVALID_CHANNEL_MODE:
		return BLUEZ_ERROR_A2DP_INVALID_CHANNEL_MODE;
	case ERROR_CODE_A2DP_INVALID_SAMPLE_RATE:
		return BLUEZ_ERROR_A2DP_INVALID_SAMPLING_FREQ;
	case ERROR_CODE_A2DP_INVALID_BLOCK_LENGTH:
		return BLUEZ_ERROR_A2DP_INVALID_BLOCK_LENGTH;
	case ERROR_CODE_A2DP_INVALID_SUB_BANDS:
		return BLUEZ_ERROR_A2DP_INVALID_SUB_BANDS;
	case ERROR_CODE_A2DP_INVALID_ALLOCATION_METHOD:
		return BLUEZ_ERROR_A2DP_INVALID_ALLOC_METHOD;
	case ERROR_CODE_A2DP_INVALID_MIN_BIT_POOL_VALUE:
		return BLUEZ_ERROR_A2DP_INVALID_MIN_BIT_POOL;
	case ERROR_CODE_A2DP_INVALID_MAX_BIT_POOL_VALUE:
		return BLUEZ_ERROR_A2DP_INVALID_MAX_BIT_POOL;
	case ERROR_CODE_A2DP_INVALID_LAYER:
		return BLUEZ_ERROR_A2DP_INVALID_LAYER;
	case ERROR_CODE_A2DP_INVALID_OBJECT_TYPE:
		return BLUEZ_ERROR_A2DP_INVALID_OBJECT_TYPE;
	default:
		return BLUEZ_ERROR_A2DP_INVALID_CODEC_PARAM;
	}
}

static void bluez_endpoint_select_configuration(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;
	const struct a2dp_sep *sep = dbus_obj->sep;

	const void *data;
	a2dp_t capabilities = { 0 };
	size_t size = 0;

	params = g_variant_get_child_value(params, 0);
	data = g_variant_get_fixed_array(params, &size, sizeof(char));
	memcpy(&capabilities, data, MIN(size, sizeof(capabilities)));
	g_variant_unref(params);

	hexdump("A2DP peer capabilities blob", &capabilities, size);
	if (a2dp_select_configuration(sep, &capabilities, size) != ERROR_CODE_OK)
		goto fail;

	GVariant * rv = g_variant_new_fixed_byte_array(&capabilities, size);
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(&rv, 1));

	return;

fail:
	g_dbus_method_invocation_return_dbus_error(inv,
			BLUEZ_ERROR_FAILED, "Invalid capabilities");
}

static void bluez_endpoint_set_configuration(GDBusMethodInvocation *inv, void *userdata) {

	const char *sender = g_dbus_method_invocation_get_sender(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;
	const char * dbus_error = BLUEZ_ERROR_FAILED;
	const struct a2dp_sep *sep = dbus_obj->sep;

	struct ba_adapter *a = NULL;
	struct ba_transport *t = NULL;
	struct ba_device *d = NULL;

	enum bluez_media_transport_state state = 0xFFFF;
	char *device_path = NULL;
	a2dp_t configuration = { 0 };
	bool delay_reporting = false;
	uint16_t volume = 127;
	uint16_t delay = 150;

	const char *transport_path;
	GVariantIter *properties;
	GVariant *value = NULL;
	const char *property;

	g_variant_get(params, "(&oa{sv})", &transport_path, &properties);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {

		if (strcmp(property, "Device") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_OBJECT_PATH, property)) {
			g_free(device_path);
			device_path = g_variant_dup_string(value, NULL);
		}
		else if (strcmp(property, "UUID") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
		}
		else if (strcmp(property, "Codec") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTE, property)) {

			const uint8_t codec_value = g_variant_get_byte(value);
			const uint8_t codec_value_ok = bluez_get_media_endpoint_codec(sep);
			if (codec_value != codec_value_ok) {
				error("Invalid configuration: %s: %u != %u",
						"Codec mismatch", codec_value, codec_value_ok);
				dbus_error = BLUEZ_ERROR_A2DP_INVALID_CODEC_TYPE;
				goto fail;
			}

		}
		else if (strcmp(property, "Configuration") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTESTRING, property)) {

			size_t size = 0;
			const void *data = g_variant_get_fixed_array(value, &size, sizeof(char));
			memcpy(&configuration, data, MIN(size, sizeof(configuration)));

			error_code_t err;
			if ((err = a2dp_check_configuration(sep, data, size)) != ERROR_CODE_OK) {
				error("Invalid configuration: %s: %s",
						"Invalid configuration blob", error_code_strerror(err));
				dbus_error = media_endpoint_error_from_error_code(err);
				goto fail;
			}

		}
		else if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
			state = media_transport_state_from_string(g_variant_get_string(value, NULL));
		}
		else if (strcmp(property, "Delay") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			delay = g_variant_get_uint16(value);
			delay_reporting = true;
		}
		else if (strcmp(property, "Volume") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			/* received volume is in range [0, 127] */
			volume = g_variant_get_uint16(value);
		}

		g_variant_unref(value);
		value = NULL;
	}

	if (state == 0xFFFF) {
		error("Invalid configuration: %s", "Missing state");
		dbus_error = BLUEZ_ERROR_INVALID_ARGUMENTS;
		goto fail;
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

	if (d->sep_configs == NULL) {
		struct bluez_adapter *b_adapter = &bluez_adapters[a->hci.dev_id];
		d->sep_configs = bluez_adapter_get_device_sep_configs(b_adapter, &addr);
	}

	if ((t = ba_transport_lookup(d, transport_path)) != NULL &&
			dbus_obj->connected) {
		error("Transport already configured: %s", transport_path);
		goto fail;
	}

	if ((t = ba_transport_new_a2dp(d, dbus_obj->profile,
					sender, transport_path, sep, &configuration)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	/* Skip volume level initialization in case of A2DP/ASHA Source
	 * profile and software volume control. */
	if (!(t->profile & (
					BA_TRANSPORT_PROFILE_A2DP_SOURCE |
#if ENABLE_ASHA
					BA_TRANSPORT_PROFILE_ASHA_SOURCE |
#endif
					0) && t->media.pcm.soft_volume)) {

		/* Volume rage for A2DP and ASHA is the same. */
		int level = ba_transport_pcm_volume_range_to_level(volume,
				BLUEZ_MEDIA_TRANSPORT_A2DP_VOLUME_MAX);

		pthread_mutex_lock(&t->media.pcm.mutex);
		for (size_t i = 0; i < t->media.pcm.channels; i++)
			ba_transport_pcm_volume_set(&t->media.pcm.volume[i], &level, NULL, NULL);
		pthread_mutex_unlock(&t->media.pcm.mutex);

	}

	t->media.bluez_dbus_sep_path = dbus_obj->path;
	t->media.delay_reporting = delay_reporting;
	t->media.delay = delay;
	t->media.volume = volume;

	debug("%s configured for device %s",
			ba_transport_debug_name(t),
			batostr_(&d->addr));
	hexdump("A2DP selected configuration blob",
			&configuration, sep->config.caps_size);
	debug("PCM configuration: channels=%u rate=%u",
			t->media.pcm.channels, t->media.pcm.rate);
	debug("Delay reporting: %s",
			delay_reporting ? "supported" : "unsupported");

	ba_transport_set_media_state(t, state);

	bluez_dbus_object_data_device_set(dbus_obj, d);
	dbus_obj->connected = true;

	g_dbus_method_invocation_return_value(inv, NULL);
	bluez_adapter_export_a2dp_all(&bluez_adapters[a->hci.dev_id]);
	goto final;

fail:
	g_dbus_method_invocation_return_dbus_error(inv,
			dbus_error, "Unable to set configuration");

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
}

static void bluez_endpoint_clear_configuration(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	debug("Disconnecting media endpoint: %s", dbus_obj->path);

	bluez_dbus_object_data_device_set(dbus_obj, NULL);
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

	struct bluez_dbus_object_data *dbus_obj = userdata;

	debug("Releasing media endpoint: %s", dbus_obj->path);

	bluez_dbus_object_data_device_set(dbus_obj, NULL);
	dbus_obj->connected = false;
	dbus_obj->registered = false;

	g_object_unref(inv);
}

static GVariant *bluez_media_endpoint_iface_get_property(
		const char *property, GError **error, void *userdata) {
	(void)error;

	const struct bluez_dbus_object_data *dbus_obj = userdata;
	const char *uuid = dbus_obj->profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE ?
		BT_UUID_A2DP_SOURCE : BT_UUID_A2DP_SINK;
	const struct a2dp_sep *sep = dbus_obj->sep;

	if (strcmp(property, "UUID") == 0)
		return g_variant_new_string(uuid);
	if (strcmp(property, "Codec") == 0)
		return g_variant_new_byte(bluez_get_media_endpoint_codec(sep));
	if (strcmp(property, "Vendor") == 0) {
		if (sep->config.codec_id < A2DP_CODEC_VENDOR)
			goto unavailable;
		return g_variant_new_uint32(sep->config.codec_id);
	}
	if (strcmp(property, "Capabilities") == 0)
		return g_variant_new_fixed_byte_array(&sep->config.capabilities, sep->config.caps_size);
	if (strcmp(property, "Device") == 0) {
		if (!dbus_obj->connected)
			goto unavailable;
		return g_variant_new_object_path(dbus_obj->device->bluez_dbus_path);
	}
	if (strcmp(property, "DelayReporting") == 0)
		return g_variant_new_boolean(TRUE);

	g_assert_not_reached();
	return NULL;

unavailable:
	if (error != NULL)
		*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
				"No such property '%s'", property);
	return NULL;
}

/**
 * Export A2DP endpoint. */
static void bluez_adapter_export_a2dp(
		const struct bluez_adapter * b_adapter,
		const struct a2dp_sep * sep) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "SelectConfiguration",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_endpoint_select_configuration },
		{ .method = "SetConfiguration",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_endpoint_set_configuration },
		{ .method = "ClearConfiguration",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_endpoint_clear_configuration },
		{ .method = "Release",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_endpoint_release },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
		.get_property = bluez_media_endpoint_iface_get_property,
	};

	pthread_mutex_lock(&bluez_mutex);

	struct ba_adapter * adapter = b_adapter->adapter;
	GDBusObjectManagerServer * manager = b_adapter->manager_media_application;
	enum ba_transport_profile profile = sep->config.type == A2DP_SOURCE ?
			BA_TRANSPORT_PROFILE_A2DP_SOURCE : BA_TRANSPORT_PROFILE_A2DP_SINK;

	unsigned int connected = 0;
	unsigned int index = 0;

	for (;;) {

		struct bluez_dbus_object_data *dbus_obj;
		GError *err = NULL;

		const char *path = bluez_get_media_endpoint_object_path(adapter, sep, ++index);
		if ((dbus_obj = g_hash_table_lookup(dbus_object_data_map, path)) == NULL) {

			/* End the loop if all previously created media endpoints are exported
			 * and we've got at least N not connected endpoints. */
			if (index > connected + 2)
				break;

			debug("Exporting media endpoint object: %s", path);

			if ((dbus_obj = calloc(1, sizeof(*dbus_obj))) == NULL) {
				warn("Couldn't export media endpoint: %s", strerror(errno));
				goto fail;
			}

			strncpy(dbus_obj->path, path, sizeof(dbus_obj->path));
			dbus_obj->index = index;
			dbus_obj->hci_dev_id = adapter->hci.dev_id;
			dbus_obj->sep = sep;
			dbus_obj->profile = profile;
			dbus_obj->registered = true;

			GDBusObjectSkeleton *skeleton;
			if ((skeleton = g_dbus_object_skeleton_new(path)) == NULL) {
				free(dbus_obj);
				goto fail;
			}

			OrgBluezMediaEndpoint1Skeleton *ifs_endpoint;
			if ((ifs_endpoint = org_bluez_media_endpoint1_skeleton_new(&vtable,
							dbus_obj, NULL)) == NULL) {
				g_object_unref(skeleton);
				free(dbus_obj);
				goto fail;
			}

			GDBusInterfaceSkeleton *ifs = G_DBUS_INTERFACE_SKELETON(ifs_endpoint);
			g_dbus_object_skeleton_add_interface(skeleton, ifs);
			g_object_unref(ifs_endpoint);

			g_dbus_object_manager_server_export(manager, skeleton);
			g_object_unref(skeleton);

			g_hash_table_insert(dbus_object_data_map, dbus_obj->path, dbus_obj);

		}

		if (dbus_obj->connected)
			connected++;

		continue;

fail:
		if (err != NULL) {
			warn("Couldn't export media endpoint: %s", err->message);
			g_error_free(err);
		}
	}

	pthread_mutex_unlock(&bluez_mutex);

}

/**
 * Export all A2DP endpoints. */
static void bluez_adapter_export_a2dp_all(struct bluez_adapter * b_adapter) {
	struct a2dp_sep * const * seps = a2dp_seps;
	for (const struct a2dp_sep *sep = *seps; sep != NULL; sep = *++seps) {
		if (!sep->enabled)
			continue;
		bluez_adapter_export_a2dp(b_adapter, sep);
	}
}

static GVariant *bluez_battery_provider_iface_skeleton_get_property(
		const char *property, GError **error, void *userdata) {
	(void)error;

	const struct ba_device *d = userdata;

	if (strcmp(property, "Device") == 0)
		return ba_variant_new_device_path(d);
	if (strcmp(property, "Percentage") == 0)
		return ba_variant_new_device_battery(d);
	if (strcmp(property, "Source") == 0)
		return g_variant_new_string("BlueALSA");

	g_assert_not_reached();
	return NULL;
}

/**
 * Add battery to battery provider. */
static bool bluez_manager_battery_add(struct ba_device *device) {

	static const GDBusInterfaceSkeletonVTable vtable = {
		.get_property = bluez_battery_provider_iface_skeleton_get_property,
	};

	struct ba_adapter *a = device->a;
	GDBusObjectManagerServer *manager = bluez_adapters[a->hci.dev_id].manager_battery_provider;

	if (device->ba_battery_dbus_path != NULL)
		return true;

	GDBusObjectSkeleton *skeleton = NULL;
	OrgBluezBatteryProvider1Skeleton *ifs_battery_provider = NULL;

	char *path = g_strdup_printf("/org/bluez/%s/battery/%s",
			a->hci.name, device->addr_dbus_str);
	if ((skeleton = g_dbus_object_skeleton_new(path)) == NULL)
		goto fail;
	if ((ifs_battery_provider = org_bluez_battery_provider1_skeleton_new(&vtable,
				device, (GDestroyNotify)ba_device_unref)) == NULL)
		goto fail;

	ba_device_ref(device);

	g_dbus_object_skeleton_add_interface(skeleton,
			G_DBUS_INTERFACE_SKELETON(ifs_battery_provider));
	g_object_unref(ifs_battery_provider);

	debug("Adding battery to battery provider: %s", path);

	device->ba_battery_dbus_path = path;

	g_dbus_object_manager_server_export(manager, skeleton);
	g_object_unref(skeleton);

	return true;

fail:
	if (skeleton != NULL)
		g_object_unref(skeleton);
	if (ifs_battery_provider != NULL)
		g_object_unref(ifs_battery_provider);
	g_free(path);
	return false;
}

/**
 * Remove battery from battery provider. */
static bool bluez_manager_battery_remove(struct ba_device *device) {

	struct ba_adapter *a = device->a;
	GDBusObjectManagerServer *manager = bluez_adapters[a->hci.dev_id].manager_battery_provider;

	if (device->ba_battery_dbus_path == NULL)
		return true;

	char *path = device->ba_battery_dbus_path;
	device->ba_battery_dbus_path = NULL;

	debug("Removing battery from battery provider: %s", path);
	g_dbus_object_manager_server_unexport(manager, path);
	g_free(path);

	return true;
}

static void bluez_profile_new_connection(GDBusMethodInvocation *inv, void *userdata) {

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	const char *sender = g_dbus_method_invocation_get_sender(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *device_path;
	GVariantIter *properties;
	GError *err = NULL;
	int fd = -1;

	g_variant_get(params, "(&oha{sv})", &device_path, NULL, &properties);

	GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(msg);
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

	if ((t = ba_transport_new_sco(d, dbus_obj->profile,
					sender, device_path, fd)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	error_code_t ec;
	if (((ec = sco_setup_connection_dispatcher(a))) != ERROR_CODE_OK) {
		error("Couldn't setup SCO connection dispatcher: %s", error_code_strerror(ec));
		goto fail;
	}

	debug("%s configured for device %s",
			ba_transport_debug_name(t),
			batostr_(&d->addr));

	dbus_obj->connected = true;

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_dbus_error(inv,
			BLUEZ_ERROR_FAILED, "Unable to connect profile");
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
	struct bluez_dbus_object_data *dbus_obj = userdata;

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

	struct bluez_dbus_object_data *dbus_obj = userdata;

	debug("Releasing hands-free profile: %s", dbus_obj->path);
	dbus_obj->connected = false;
	dbus_obj->registered = false;

	g_object_unref(inv);
}

/**
 * Export HFP/HSP profile. */
static struct bluez_dbus_object_data * export_profile(
		enum ba_transport_profile profile) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "NewConnection",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_profile_new_connection },
		{ .method = "RequestDisconnection",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_profile_request_disconnection },
		{ .method = "Release",
			.sender = bluez_dbus_unique_name,
			.handler = bluez_profile_release },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
	};

	pthread_mutex_lock(&bluez_mutex);

	struct bluez_dbus_object_data * obj;
	const char * path = bluez_get_profile_object_path(profile);
	if ((obj = g_hash_table_lookup(dbus_object_data_map, path)) == NULL) {

		debug("Exporting hands-free profile object: %s", path);

		if ((obj = calloc(1, sizeof(*obj))) == NULL) {
			warn("Couldn't export hands-free profile: %s", strerror(errno));
			goto fail;
		}

		strncpy(obj->path, path, sizeof(obj->path));
		obj->hci_dev_id = -1;
		obj->profile = profile;

		g_autoptr(OrgBluezProfile1Skeleton) ifs;
		ifs = org_bluez_profile1_skeleton_new(&vtable, obj, NULL);

		g_autoptr(GError) err = NULL;
		obj->ifs = G_DBUS_INTERFACE_SKELETON(ifs);
		if (!g_dbus_interface_skeleton_export(obj->ifs, config.dbus, path, &err)) {
			warn("Couldn't export hands-free profile: %s", err->message);
			free(g_steal_pointer(&obj));
			goto fail;
		}

		g_hash_table_insert(dbus_object_data_map, obj->path, obj);
		ifs = NULL;

	}

fail:
	pthread_mutex_unlock(&bluez_mutex);

	return obj;
}

static void register_profile_finish(
		GObject * source,
		GAsyncResult * result,
		void * userdata) {
	struct bluez_dbus_object_data * obj = userdata;

	g_autoptr(GDBusMessage) rep;
	g_autoptr(GError) err = NULL;
	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	if (err != NULL)
		error("Couldn't register hands-free profile: %s", err->message);
	else {
		obj->registered = true;
	}

}

/**
 * Register Bluetooth Hands-Free Audio Profile. */
static void bluez_register_hfp(
		const char * uuid,
		enum ba_transport_profile profile,
		uint16_t version,
		uint16_t features) {
	struct bluez_dbus_object_data * obj;
	if ((obj = export_profile(profile)) != NULL &&
			!obj->registered) {

		g_autoptr(GDBusMessage) msg = g_dbus_message_new_method_call(BLUEZ_SERVICE,
				"/org/bluez", BLUEZ_IFACE_PROFILE_MANAGER, "RegisterProfile");

		GVariantBuilder options;
		g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

		if (version)
			g_variant_builder_add(&options, "{sv}", "Version", g_variant_new_uint16(version));
		if (features)
			g_variant_builder_add(&options, "{sv}", "Features", g_variant_new_uint16(features));

		g_dbus_message_set_body(msg, g_variant_new("(osa{sv})", obj->path, uuid, &options));
		g_variant_builder_clear(&options);

		debug("Registering hands-free profile: %s", obj->path);
		g_dbus_connection_send_message_with_reply(config.dbus, msg,
				G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
				register_profile_finish, obj);

	}
}

/**
 * Register Bluetooth Hands-Free Audio Profiles.
 *
 * This function also registers deprecated HSP profile. Profiles registration
 * is controlled by the global configuration structure - if none is enabled,
 * this function will do nothing. */
static void bluez_register_hfp_all(void) {
	if (config.profile.hsp_hs)
		bluez_register_hfp(BT_UUID_HSP_HS, BA_TRANSPORT_PROFILE_HSP_HS,
				0x0102 /* HSP 1.2 */, 0x1 /* remote audio volume control */);
	if (config.profile.hsp_ag)
		bluez_register_hfp(BT_UUID_HSP_AG, BA_TRANSPORT_PROFILE_HSP_AG,
				0x0102 /* HSP 1.2 */, 0x0);
	if (config.profile.hfp_hf)
		bluez_register_hfp(BT_UUID_HFP_HF, BA_TRANSPORT_PROFILE_HFP_HF,
				0x0109 /* HFP 1.9 */, ba_config_get_hfp_sdp_features_hf());
	if (config.profile.hfp_ag)
		bluez_register_hfp(BT_UUID_HFP_AG, BA_TRANSPORT_PROFILE_HFP_AG,
				0x0109 /* HFP 1.9 */, ba_config_get_hfp_sdp_features_ag());
}

static void bluez_media_endpoint_process_a2dp(
		struct bluez_adapter *b_adapter,
		const char *media_endpoint_path,
		GVariantIter *properties,
		enum a2dp_type type) {

	struct a2dp_sep_config sep_cfg = { .type = type };
	const char *property;
	GVariant *value;

	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
		if (strcmp(property, "Codec") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTE, property))
			sep_cfg.codec_id = g_variant_get_byte(value);
		else if (strcmp(property, "Capabilities") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTESTRING, property)) {

			const void *data = g_variant_get_fixed_array(value,
					&sep_cfg.caps_size, sizeof(char));

			if (sep_cfg.caps_size > sizeof(sep_cfg.capabilities)) {
				warn("Capabilities blob size exceeded: %zu > %zu",
						sep_cfg.caps_size, sizeof(sep_cfg.capabilities));
				sep_cfg.caps_size = sizeof(sep_cfg.capabilities);
			}

			memcpy(&sep_cfg.capabilities, data, sep_cfg.caps_size);

		}
		g_variant_unref(value);
	}

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(media_endpoint_path, &addr);

	strncpy(sep_cfg.bluez_dbus_path, media_endpoint_path, sizeof(sep_cfg.bluez_dbus_path) - 1);
	if (sep_cfg.codec_id == A2DP_CODEC_VENDOR)
		sep_cfg.codec_id = a2dp_get_vendor_codec_id(&sep_cfg.capabilities, sep_cfg.caps_size);

	debug("Adding new Stream End-Point: %s: %s: %s",
			batostr_(&addr), sep_cfg.type == A2DP_SOURCE ? "SRC" : "SNK",
			a2dp_codecs_codec_id_to_string(sep_cfg.codec_id));
	hexdump("SEP capabilities blob", &sep_cfg.capabilities, sep_cfg.caps_size);

	GArray *sep_cfgs = bluez_adapter_get_device_sep_configs(b_adapter, &addr);
	g_array_append_val(sep_cfgs, sep_cfg);

	/* Collected SEPs are exposed via BlueALSA D-Bus API. We will sort them
	 * here, so the D-Bus API will return codecs in the defined order. */
	g_array_sort(sep_cfgs, (GCompareFunc)a2dp_sep_config_cmp);

}

#if ENABLE_ASHA
/**
 * Process ASHA media endpoint and create BlueALSA ASHA transport. */
static void bluez_media_endpoint_process_asha(
		struct bluez_adapter * b_adapter,
		const char * media_endpoint_path,
		GVariantIter * properties) {

	enum bluez_media_transport_state state = BLUEZ_MEDIA_TRANSPORT_STATE_IDLE;
	const char * transport_path = NULL;
	const uint8_t * id = NULL;
	size_t id_size = 0;
	bool right = false;
	bool binaural = false;
	uint16_t delay = 0;
	uint16_t volume = 0;
	uint8_t codec = 0;

	const char * property;
	GVariant * value;

	/* Get some information from the media endpoint properties. */
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
		if (strcmp(property, "HiSyncId") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTESTRING, property))
			id = g_variant_get_fixed_array(value, &id_size, sizeof(uint8_t));
		else if (strcmp(property, "Side") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property))
			right = strcmp(g_variant_get_string(value, NULL), "right") == 0;
		else if (strcmp(property, "Binaural") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BOOLEAN, property))
			binaural = g_variant_get_boolean(value);
		else if (strcmp(property, "Transport") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_OBJECT_PATH, property))
			transport_path = g_variant_get_string(value, NULL);
		g_variant_unref(value);
	}

	/* Make sure that we have all required data. */
	if (id == NULL || transport_path == NULL)
		return;

	GError * err = NULL;
	if ((properties = g_dbus_get_properties_sync(config.dbus, BLUEZ_SERVICE,
					transport_path, BLUEZ_IFACE_MEDIA_TRANSPORT, &err)) == NULL) {
		error("Couldn't get ASHA transport properties: %s", err->message);
		g_error_free(err);
		return;
	}

	/* Get more information from the media transport properties. */
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
		if (strcmp(property, "Codec") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTE, property))
			codec = g_variant_get_byte(value);
		else if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property))
			state = media_transport_state_from_string(g_variant_get_string(value, NULL));
		else if (strcmp(property, "Delay") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property))
			delay = g_variant_get_uint16(value);
		else if (strcmp(property, "Volume") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property))
			volume = g_variant_get_uint16(value);
		g_variant_unref(value);
	}

	struct ba_device * d = NULL;
	struct ba_transport * t = NULL;

	/* There is only one ASHA codec defined by the specification. */
	if (codec != ASHA_CODEC_G722) {
		error("Unsupported ASHA codec: %#x", codec);
		goto fail;
	}

	/* Make sure that HiSyncId size is valid. */
	if (id_size != sizeof(asha_hi_sync_id_t)) {
		error("Invalid HiSyncId size: %zu != %zu", id_size, sizeof(asha_hi_sync_id_t));
		goto fail;
	}

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(media_endpoint_path, &addr);
	if ((d = ba_device_lookup(b_adapter->adapter, &addr)) == NULL &&
			(d = ba_device_new(b_adapter->adapter, &addr)) == NULL) {
		error("Couldn't create new device: %s", media_endpoint_path);
		goto fail;
	}

	if ((t = ba_transport_lookup(d, transport_path)) != NULL) {
		error("Transport already configured: %s", transport_path);
		goto fail;
	}

	asha_hi_sync_id_t tmp;
	memcpy(&tmp, id, sizeof(tmp));
	if ((t = ba_transport_new_asha(d, BA_TRANSPORT_PROFILE_ASHA_SOURCE,
					BLUEZ_SERVICE, transport_path, tmp)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	t->media.delay = delay;
	t->media.volume = volume;
	t->media.asha.side = right ? ASHA_CAPABILITY_SIDE_RIGHT : ASHA_CAPABILITY_SIDE_LEFT;
	t->media.asha.binaural = binaural;

	debug("%s configured for device %s",
			ba_transport_debug_name(t),
			batostr_(&d->addr));
	debug("PCM configuration: channels=%u rate=%u",
			t->media.pcm.channels, t->media.pcm.rate);

	ba_transport_set_media_state(t, state);

fail:
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
	/* Free the media transport iterator. */
	g_variant_iter_free(properties);
}
#endif

/**
 * Register to the BlueZ service. */
static void bluez_register(void) {

	const struct {
		const char *uuid;
		enum ba_transport_profile profile;
		bool enabled;
		bool global;
	} uuids[] = {
		{ BT_UUID_A2DP_SOURCE, BA_TRANSPORT_PROFILE_A2DP_SOURCE,
			config.profile.a2dp_source, false },
		{ BT_UUID_A2DP_SINK, BA_TRANSPORT_PROFILE_A2DP_SINK,
			config.profile.a2dp_sink, false },
		{ BT_UUID_HSP_HS, BA_TRANSPORT_PROFILE_HSP_HS,
			config.profile.hsp_hs, true },
		{ BT_UUID_HSP_AG, BA_TRANSPORT_PROFILE_HSP_AG,
			config.profile.hsp_ag, true },
		{ BT_UUID_HFP_HF, BA_TRANSPORT_PROFILE_HFP_HF,
			config.profile.hfp_hf, true },
		{ BT_UUID_HFP_AG, BA_TRANSPORT_PROFILE_HFP_AG,
			config.profile.hfp_ag, true },
	};

	GError *err = NULL;
	GVariantIter *objects = NULL;
	if ((objects = g_dbus_get_managed_objects_sync(config.dbus, BLUEZ_SERVICE, "/", &err)) == NULL) {
		warn("Couldn't get managed objects: %s", err->message);
		g_error_free(err);
		return;
	}

	unsigned int profiles = 0;
	GVariantIter *interfaces;
	GVariantIter *properties;
	GVariant *value;
	const char *object_path;
	const char *interface;
	const char *property;

	while (g_variant_iter_next(objects, "{&oa{sa{sv}}}", &object_path, &interfaces)) {
		const int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);
		while (hci_dev_id != -1 &&
				g_variant_iter_next(interfaces, "{&sa{sv}}", &interface, &properties)) {
			if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0) {

				unsigned int a_profiles = 0;
				bool valid = false;

				while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
					if (strcmp(property, "Address") == 0 &&
							g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property))
						/* Check if adapter as valid for registration. */
						valid = bluez_match_dbus_adapter(object_path, g_variant_get_string(value, NULL));
					else if (strcmp(property, "UUIDs") == 0 &&
							g_variant_validate_value(value, G_VARIANT_TYPE_STRING_ARRAY, property)) {
						const char **value_uuids = g_variant_get_strv(value, NULL);
						/* Map UUIDs to BlueALSA transport profile mask. */
						for (size_t i = 0; value_uuids[i] != NULL; i++)
							for (size_t ii = 0; ii < ARRAYSIZE(uuids); ii++)
								if (strcasecmp(value_uuids[i], uuids[ii].uuid) == 0)
									a_profiles |= uuids[ii].profile;
						g_free(value_uuids);
					}
					g_variant_unref(value);
				}

				profiles |= a_profiles;

				struct ba_adapter *a;
				if (valid && (
						(a = ba_adapter_lookup(hci_dev_id)) != NULL ||
						(a = ba_adapter_new(hci_dev_id)) != NULL)) {

					for (size_t i = 0; i < ARRAYSIZE(uuids); i++)
						if (uuids[i].enabled && !uuids[i].global && a_profiles & uuids[i].profile)
							warn("UUID already registered in BlueZ [%s]: %s", a->hci.name, uuids[i].uuid);

					struct bluez_adapter * b_adapter = bluez_adapter_new(a);
					bluez_adapter_register_battery_provider(b_adapter);
					bluez_adapter_register_media_application(b_adapter);
					bluez_adapter_register_gatt_applications(b_adapter);

				}

			}
			else {

				/* Validate that this new interface belongs to the HCI
				 * which matches our HCI filter. */
				struct bluez_adapter * b_adapter = &bluez_adapters[hci_dev_id];
				if (b_adapter->adapter == NULL)
					continue;

#if ENABLE_ASHA
				if (config.profile.asha_source) {
					bool processed = false;
					while (!processed && g_variant_iter_next(properties, "{&sv}", &property, &value)) {
						if (strcmp(property, "UUID") == 0 &&
								g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property) &&
								strcasecmp(g_variant_get_string(value, NULL), BT_UUID_ASHA) == 0) {
							/* Check for existing ASHA transport and process it. */
							bluez_media_endpoint_process_asha(b_adapter, object_path, properties);
							processed = true;
						}
						g_variant_unref(value);
					}
				}
#endif

			}
			g_variant_iter_free(properties);
		}
		g_variant_iter_free(interfaces);
	}
	g_variant_iter_free(objects);

	for (size_t i = 0; i < ARRAYSIZE(uuids); i++)
		if (uuids[i].enabled && uuids[i].global && profiles & uuids[i].profile)
			warn("UUID already registered in BlueZ: %s", uuids[i].uuid);

	/* HFP has to be registered globally. */
	bluez_register_hfp_all();

}

static void bluez_signal_interfaces_added(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface_;
	(void)signal;
	(void)userdata;

	GVariantIter *interfaces;
	GVariantIter *properties;
	GVariant *value;
	const char *object_path;
	const char *interface;
	const char *property;

	g_variant_get(params, "(&oa{sa{sv}})", &object_path, &interfaces);
	debug("Signal: %s.%s(%s, ...)", interface_, signal, object_path);

	const int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);
	while (g_variant_iter_next(interfaces, "{&sa{sv}}", &interface, &properties)) {
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0) {
			while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
				if (strcmp(property, "Address") == 0 &&
						g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property) &&
						/* Make sure that this new BT adapter matches our HCI filter. */
						bluez_match_dbus_adapter(object_path, g_variant_get_string(value, NULL))) {

					/* In case of D-Bus service restart, the unique name changes. Since
					 * BlueALSA uses BlueZ unique name to validate D-Bus method callers,
					 * we have to update it in such case. After BlueZ restart, adapters
					 * are added again, so we can update the unique name here. */
					strncpy(bluez_dbus_unique_name, sender, sizeof(bluez_dbus_unique_name) - 1);

					struct ba_adapter * a;
					if ((a = ba_adapter_lookup(hci_dev_id)) != NULL ||
							(a = ba_adapter_new(hci_dev_id)) != NULL) {
						bluez_adapter_new(a);
					}

				}
				g_variant_unref(value);
			}
		}
		else {

			/* Check whether this new interface was added on the HCI which exists
			 * in our local BlueZ adapter cache - HCI that matches our HCI filter. */
			struct bluez_adapter * b_adapter = &bluez_adapters[hci_dev_id];
			if (b_adapter->adapter == NULL)
				continue;

			if (strcmp(interface, BLUEZ_IFACE_BATTERY_PROVIDER_MANAGER) == 0)
				bluez_adapter_register_battery_provider(b_adapter);
			else if (strcmp(interface, BLUEZ_IFACE_MEDIA) == 0)
				bluez_adapter_register_media_application(b_adapter);
			/* GATT applications require GATT Manager and LE Advertising Manager
			 * interfaces to be present. But it seems that BlueZ does not export
			 * them at the same time. The LE Advertising Manager appears a bit
			 * later. Hence, we are waiting for the latter one. */
			else if (strcmp(interface, BLUEZ_IFACE_LE_ADVERTISING_MANAGER) == 0)
				bluez_adapter_register_gatt_applications(b_adapter);
			else if (strcmp(interface, BLUEZ_IFACE_MEDIA_ENDPOINT) == 0) {
				bool processed = false;
				while (!processed && g_variant_iter_next(properties, "{&sv}", &property, &value)) {
					if (strcmp(property, "UUID") == 0 &&
							g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
						const char *uuid = g_variant_get_string(value, NULL);
						if (strcasecmp(uuid, BT_UUID_A2DP_SOURCE) == 0)
							bluez_media_endpoint_process_a2dp(b_adapter, object_path, properties, A2DP_SOURCE);
						else if (strcasecmp(uuid, BT_UUID_A2DP_SINK) == 0)
							bluez_media_endpoint_process_a2dp(b_adapter, object_path, properties, A2DP_SINK);
#if ENABLE_ASHA
						else if (config.profile.asha_source &&
								strcasecmp(uuid, BT_UUID_ASHA) == 0)
							bluez_media_endpoint_process_asha(b_adapter, object_path, properties);
#endif
						processed = true;
					}
					g_variant_unref(value);
				}
			}

		}
		g_variant_iter_free(properties);
	}
	g_variant_iter_free(interfaces);

	/* HFP has to be registered globally. */
	if (strcmp(object_path, "/org/bluez") == 0) {
		bluez_register_hfp_all();
	}

}

static void bluez_signal_interfaces_removed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface_, const char *signal, GVariant *params,
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
	int hci_dev_id;

	g_variant_get(params, "(&oas)", &object_path, &interfaces);
	debug("Signal: %s.%s(%s, ...)", interface_, signal, object_path);

	hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);

	pthread_mutex_lock(&bluez_mutex);

	/* Check whether this interface belongs to a HCI which exists in our
	 * local BlueZ adapter cache - HCI that matches our HCI filter. */
	if (hci_dev_id == -1 || bluez_adapters[hci_dev_id].adapter == NULL)
		goto final;

	while (g_variant_iter_next(interfaces, "&s", &interface))
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0) {

			GHashTableIter iter;
			struct bluez_dbus_object_data *dbus_obj;
			g_hash_table_iter_init(&iter, dbus_object_data_map);
			while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj)) {
				if (dbus_obj->hci_dev_id != hci_dev_id)
					continue;
				g_hash_table_iter_remove(&iter);
			}

			bluez_adapter_free(&bluez_adapters[hci_dev_id]);

		}
		else if (strcmp(interface, BLUEZ_IFACE_MEDIA_ENDPOINT) == 0) {

			bdaddr_t addr;
			g_dbus_bluez_object_path_to_bdaddr(object_path, &addr);
			struct bluez_adapter *b_adapter = &bluez_adapters[hci_dev_id];
			GArray *sep_cfgs = bluez_adapter_get_device_sep_configs(b_adapter, &addr);

			for (size_t i = 0; i < sep_cfgs->len; i++) {
				const struct a2dp_sep_config *sep_cfg = &ba_device_sep_cfg_array_index(sep_cfgs, i);
				if (strcmp(sep_cfg->bluez_dbus_path, object_path) == 0) {
					debug("Removing Stream End-Point: %s: %s: %s",
							batostr_(&addr), sep_cfg->type == A2DP_SOURCE ? "SRC" : "SNK",
							a2dp_codecs_codec_id_to_string(sep_cfg->codec_id));
					g_array_remove_index_fast(sep_cfgs, i);
				}
			}

		}
		else if (strcmp(interface, BLUEZ_IFACE_MEDIA_TRANSPORT) == 0) {

			bdaddr_t addr;
			g_dbus_bluez_object_path_to_bdaddr(object_path, &addr);
			struct bluez_adapter * b_adapter = &bluez_adapters[hci_dev_id];

			struct ba_device * d;
			if ((d = ba_device_lookup(b_adapter->adapter, &addr)) != NULL) {
				struct ba_transport * t;
				/* BlueALSA transport can not operate without BlueZ media transport. */
				if ((t = ba_transport_lookup(d, object_path)) != NULL)
					ba_transport_destroy(t);
				ba_device_unref(d);
			}

		}

final:
	pthread_mutex_unlock(&bluez_mutex);
	g_variant_iter_free(interfaces);
}

static void bluez_signal_transport_changed(GDBusConnection *conn, const char *sender,
		const char *transport_path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface_;
	(void)signal;
	(void)userdata;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(transport_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL) {
		error("Adapter not available: %s", transport_path);
		goto fail;
	}

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(transport_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL) {
		error("Device not available: %s", transport_path);
		goto fail;
	}

	if ((t = ba_transport_lookup(d, transport_path)) == NULL) {
		error("Transport not available: %s", transport_path);
		goto fail;
	}

	GVariantIter *properties;
	const char *interface;
	const char *property;
	GVariant *value;

	g_variant_get(params, "(&sa{sv}as)", &interface, &properties, NULL);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
		debug("Signal: %s.%s(): %s: %s", interface_, signal, interface, property);

		if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
			const char *state = g_variant_get_string(value, NULL);
			ba_transport_set_media_state(t, media_transport_state_from_string(state));
		}
		else if (strcmp(property, "Delay") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			t->media.delay = g_variant_get_uint16(value);
			bluealsa_dbus_pcm_update(&t->media.pcm, BA_DBUS_PCM_UPDATE_DELAY);
		}
		else if (strcmp(property, "Volume") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			/* received volume is in range [0, 127] */
			uint16_t volume = t->media.volume = g_variant_get_uint16(value);
			if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE &&
					t->media.pcm.soft_volume)
				debug("Skipping A2DP volume update: %u", volume);
			else {

				int level = ba_transport_pcm_volume_range_to_level(volume,
						BLUEZ_MEDIA_TRANSPORT_A2DP_VOLUME_MAX);
				debug("Updating A2DP volume: %u [%.2f dB]", volume, 0.01 * level);

				pthread_mutex_lock(&t->media.pcm.mutex);
				for (size_t i = 0; i < t->media.pcm.channels; i++)
					ba_transport_pcm_volume_set(&t->media.pcm.volume[i], &level, NULL, NULL);
				pthread_mutex_unlock(&t->media.pcm.mutex);

				bluealsa_dbus_pcm_update(&t->media.pcm, BA_DBUS_PCM_UPDATE_VOLUME);

			}
		}

		g_variant_unref(value);
	}
	g_variant_iter_free(properties);

fail:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
}

/**
 * Monitor BlueZ service disappearance.
 *
 * When BlueZ is properly shutdown, we are notified about adapter removal via
 * the InterfacesRemoved signal. Here, we get the opportunity to perform some
 * cleanup if BlueZ service was killed. */
static void bluez_disappeared(GDBusConnection *conn, const char *name,
		void *userdata) {
	(void)conn;
	(void)name;
	(void)userdata;

	pthread_mutex_lock(&bluez_mutex);

	g_hash_table_remove_all(dbus_object_data_map);

	for (size_t i = 0; i < ARRAYSIZE(bluez_adapters); i++)
		bluez_adapter_free(&bluez_adapters[i]);

	pthread_mutex_unlock(&bluez_mutex);

}

static unsigned int bluez_sig_sub_id_iface_added = 0;
static unsigned int bluez_sig_sub_id_iface_removed = 0;
static unsigned int bluez_sig_sub_id_prop_changed = 0;
static unsigned int bluez_bus_watch_id = 0;

/**
 * Subscribe to BlueZ signals. */
static void bluez_signals_subscribe(void) {

	bluez_sig_sub_id_iface_added = g_dbus_connection_signal_subscribe(config.dbus,
			BLUEZ_SERVICE, DBUS_IFACE_OBJECT_MANAGER, "InterfacesAdded", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_interfaces_added, NULL, NULL);
	bluez_sig_sub_id_iface_removed = g_dbus_connection_signal_subscribe(config.dbus,
			BLUEZ_SERVICE, DBUS_IFACE_OBJECT_MANAGER, "InterfacesRemoved", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_interfaces_removed, NULL, NULL);

	bluez_sig_sub_id_prop_changed = g_dbus_connection_signal_subscribe(config.dbus,
			BLUEZ_SERVICE, DBUS_IFACE_PROPERTIES, "PropertiesChanged", NULL, BLUEZ_IFACE_MEDIA_TRANSPORT,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_transport_changed, NULL, NULL);

	bluez_bus_watch_id = g_bus_watch_name_on_connection(config.dbus,
			BLUEZ_SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE, NULL, bluez_disappeared,
			NULL, NULL);

}

static void bluez_signals_unsubscribe(void) {
	g_dbus_connection_signal_unsubscribe(config.dbus, bluez_sig_sub_id_iface_added);
	g_dbus_connection_signal_unsubscribe(config.dbus, bluez_sig_sub_id_iface_removed);
	g_dbus_connection_signal_unsubscribe(config.dbus, bluez_sig_sub_id_prop_changed);
	g_bus_unwatch_name(bluez_bus_watch_id);
}

/**
 * Initialize integration with BlueZ service.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_init(void) {

	dbus_object_data_map = g_hash_table_new_full(g_str_hash, g_str_equal,
			NULL, (GDestroyNotify)bluez_dbus_object_data_free);

	char * name;
	/* Get BlueZ service D-Bus unique name for calls filtering. */
	if ((name = g_dbus_get_unique_name_sync(config.dbus, BLUEZ_SERVICE)) != NULL) {
		strncpy(bluez_dbus_unique_name, name, sizeof(bluez_dbus_unique_name) - 1);
		g_free(name);
	}

	bluez_signals_subscribe();
	bluez_register();

	return 0;
}

/**
 * Release resources associated with BlueZ service integration.
 *
 * Please note that this function does not perform full cleanup. It does not
 * unregister objects exported to the BlueZ service, so it is not possible to
 * initialize BlueZ integration again. This function should be called only to
 * release resources before exiting the application. */
void bluez_destroy(void) {

	if (dbus_object_data_map == NULL)
		return;

	bluez_signals_unsubscribe();

	for (size_t i = 0; i < ARRAYSIZE(bluez_adapters); i++)
		bluez_adapter_free(&bluez_adapters[i]);

	g_hash_table_unref(dbus_object_data_map);
	dbus_object_data_map = NULL;

}

/**
 * Set new configuration for already connected A2DP endpoint.
 *
 * @param dbus_current_sep_path D-Bus SEP path of current connection.
 * @param remote_sep_cfg New SEP to be configured.
 * @param error NULL GError pointer.
 * @return On success this function returns true. */
bool bluez_a2dp_set_configuration(
		const char *dbus_current_sep_path,
		const struct a2dp_sep_config *remote_sep_cfg,
		const void *configuration,
		GError **error) {

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(remote_sep_cfg->bluez_dbus_path);
	unsigned int index = UINT_MAX;
	const char *endpoint = NULL;
	GDBusMessage *msg = NULL;
	GDBusMessage *rep = NULL;
	bool rv = false;

	/* When requesting new A2DP configuration, BlueZ will disconnect the
	 * existing connection and will try to establish a new one. So, when
	 * changing configuration for already selected codec we can reuse the
	 * endpoint path - it will be disconnected before new connection is
	 * established. Things are more complicated when we want to select
	 * different codec. In such case we will have to choose one of the
	 * endpoint paths registered in BlueZ but not connected. The issue
	 * is, that we can not avoid race condition when new BT device will
	 * try to set configuration as well - we have to pick endpoint path
	 * currently not-used by BlueZ... */

	pthread_mutex_lock(&bluez_mutex);

	GHashTableIter iter;
	struct bluez_dbus_object_data *dbus_obj;
	g_hash_table_iter_init(&iter, dbus_object_data_map);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj))
		if (dbus_obj->hci_dev_id == hci_dev_id &&
				dbus_obj->sep->config.codec_id == remote_sep_cfg->codec_id &&
				dbus_obj->sep->config.type == !remote_sep_cfg->type &&
				dbus_obj->registered) {

			/* reuse already selected endpoint path */
			if (strcmp(dbus_obj->path, dbus_current_sep_path) == 0) {
				endpoint = dbus_obj->path;
				break;
			}

			/* select not connected endpoint with the lowest index */
			if (!dbus_obj->connected &&
					dbus_obj->index < index) {
				endpoint = dbus_obj->path;
				index = dbus_obj->index;
			}

		}

	if (endpoint == NULL) {
		pthread_mutex_unlock(&bluez_mutex);
		*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
				"Extra A2DP endpoint not available");
		goto fail;
	}

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&props, "{sv}", "Capabilities",
				g_variant_new_fixed_byte_array(configuration, remote_sep_cfg->caps_size));

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE,
			remote_sep_cfg->bluez_dbus_path, BLUEZ_IFACE_MEDIA_ENDPOINT, "SetConfiguration");
	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", endpoint, &props));
	g_variant_builder_clear(&props);

	pthread_mutex_unlock(&bluez_mutex);

	debug("A2DP requested codec: %s", a2dp_codecs_codec_id_to_string(remote_sep_cfg->codec_id));
	hexdump("A2DP requested configuration blob", configuration, remote_sep_cfg->caps_size);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL ||
			g_dbus_message_to_gerror(rep, error))
		goto fail;

	rv = true;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	return rv;
}

/**
 * Signal update on battery provider properties.
 *
 * This function makes sure that the battery provider object is
 * exported or unexported when necessary. */
void bluez_battery_provider_update(
		struct ba_device *device) {

	if (device->battery.charge == -1) {
		bluez_manager_battery_remove(device);
		return;
	}

	if (!bluez_manager_battery_add(device))
		return;

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);

	g_variant_builder_add(&props, "{sv}", "Percentage",
			ba_variant_new_device_battery(device));

	g_dbus_connection_emit_properties_changed(config.dbus, device->ba_battery_dbus_path,
			BLUEZ_IFACE_BATTERY_PROVIDER, g_variant_builder_end(&props), NULL, NULL);

}
