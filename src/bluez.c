/*
 * BlueALSA - bluez.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluez.h"
/* IWYU pragma: no_include "config.h" */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
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
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "bluez-skeleton.h"
#include "dbus.h"
#include "hci.h"
#include "sco.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

/* Compatibility patch for glib < 2.68. */
#if !GLIB_CHECK_VERSION(2, 68, 0)
# define g_memdup2 g_memdup
#endif

/**
 * Data associated with registered D-Bus object. */
struct bluez_dbus_object_data {
	/* D-Bus object registration path */
	char path[64];
	/* exported interface skeleton */
	GDBusInterfaceSkeleton *ifs;
	/* associated adapter */
	int hci_dev_id;
	/* registered profile */
	enum ba_transport_profile profile;
	/* media endpoint codec */
	const struct a2dp_codec *codec;
	/* determine whether object is registered in BlueZ */
	bool registered;
	/* determine whether object is used */
	bool connected;
	/* memory self-management */
	atomic_int ref_count;
};

/**
 * BlueALSA copy of BlueZ adapter data. */
struct bluez_adapter {
	/* reference to the adapter structure */
	struct ba_adapter *adapter;
	/* manager for battery provider objects */
	GDBusObjectManagerServer *battery_manager;
	/* array of end-points for connected devices */
	GHashTable *device_sep_map;
};

static pthread_mutex_t bluez_mutex = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *dbus_object_data_map = NULL;
static struct bluez_adapter bluez_adapters[HCI_MAX_DEV] = { 0 };

#define bluez_adapters_device_lookup(hci_dev_id, addr) \
	g_hash_table_lookup(bluez_adapters[hci_dev_id].device_sep_map, addr)
#define bluez_adapters_device_get_sep(seps, i) \
	g_array_index(seps, struct a2dp_sep, i)

static void bluez_register_a2dp_all(struct ba_adapter *);
static void bluez_register_battery_provider_manager(struct bluez_adapter *);

static struct bluez_adapter *bluez_adapter_new(struct ba_adapter *a) {
	bluez_adapters[a->hci.dev_id].adapter = a;
	bluez_adapters[a->hci.dev_id].device_sep_map = g_hash_table_new_full(
			g_bdaddr_hash, g_bdaddr_equal, g_free, (GDestroyNotify)g_array_unref);
	bluez_register_battery_provider_manager(&bluez_adapters[a->hci.dev_id]);
	bluez_register_a2dp_all(a);
	return &bluez_adapters[a->hci.dev_id];
}

static void bluez_adapter_free(struct bluez_adapter *adapter) {
	if (adapter->adapter == NULL)
		return;
	g_hash_table_destroy(adapter->device_sep_map);
	adapter->device_sep_map = NULL;
	g_object_unref(adapter->battery_manager);
	adapter->battery_manager = NULL;
	ba_adapter_destroy(adapter->adapter);
	adapter->adapter = NULL;
}

static void bluez_dbus_object_data_unref(
		struct bluez_dbus_object_data *obj) {
	if (atomic_fetch_sub_explicit(&obj->ref_count, 1, memory_order_relaxed) > 1)
		return;
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

	size_t i;
	for (i = 0; i < config.hci_filter->len; i++)
		if (strcasecmp(adapter_path, g_array_index(config.hci_filter, char *, i)) == 0 ||
				strcasecmp(adapter_address, g_array_index(config.hci_filter, char *, i)) == 0)
			return true;

	return false;
}

/**
 * Get BlueZ D-Bus object path for given transport profile. */
static const char *bluez_transport_profile_to_bluez_object_path(
		enum ba_transport_profile profile,
		uint16_t codec_id) {
	switch (profile) {
	case BA_TRANSPORT_PROFILE_NONE:
		return "/";
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (codec_id) {
		case A2DP_CODEC_SBC:
			return "/A2DP/SBC/source";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "/A2DP/MPEG/source";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "/A2DP/AAC/source";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "/A2DP/aptX/source";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "/A2DP/aptXHD/source";
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return "/A2DP/FastStream/source";
#endif
#if ENABLE_LC3PLUS
		case A2DP_CODEC_VENDOR_LC3PLUS:
			return "/A2DP/LC3plus/source";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "/A2DP/LDAC/source";
#endif
		default:
			error("Unsupported A2DP codec: %#x", codec_id);
			g_assert_not_reached();
		}
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		switch (codec_id) {
		case A2DP_CODEC_SBC:
			return "/A2DP/SBC/sink";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "/A2DP/MPEG/sink";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "/A2DP/AAC/sink";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "/A2DP/aptX/sink";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "/A2DP/aptXHD/sink";
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return "/A2DP/FastStream/sink";
#endif
#if ENABLE_LC3PLUS
		case A2DP_CODEC_VENDOR_LC3PLUS:
			return "/A2DP/LC3plus/sink";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "/A2DP/LDAC/sink";
#endif
		default:
			error("Unsupported A2DP codec: %#x", codec_id);
			g_assert_not_reached();
		}
	case BA_TRANSPORT_PROFILE_HFP_HF:
		return "/HFP/HandsFree";
	case BA_TRANSPORT_PROFILE_HFP_AG:
		return "/HFP/AudioGateway";
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "/HSP/Headset";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "/HSP/AudioGateway";
	}
	g_assert_not_reached();
	return "/";
}

/**
 * Get transport state from BlueZ state string. */
static enum bluez_a2dp_transport_state bluez_a2dp_transport_state_from_string(
		const char *state) {
	if (strcmp(state, BLUEZ_TRANSPORT_STATE_IDLE) == 0)
		return BLUEZ_A2DP_TRANSPORT_STATE_IDLE;
	if (strcmp(state, BLUEZ_TRANSPORT_STATE_PENDING) == 0)
		return BLUEZ_A2DP_TRANSPORT_STATE_PENDING;
	if (strcmp(state, BLUEZ_TRANSPORT_STATE_ACTIVE) == 0)
		return BLUEZ_A2DP_TRANSPORT_STATE_ACTIVE;
	warn("Invalid A2DP transport state: %s", state);
	return BLUEZ_A2DP_TRANSPORT_STATE_IDLE;
}

static void bluez_endpoint_select_configuration(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;
	const struct a2dp_codec *codec = dbus_obj->codec;

	const void *data;
	a2dp_t capabilities = {};
	size_t size = 0;

	params = g_variant_get_child_value(params, 0);
	data = g_variant_get_fixed_array(params, &size, sizeof(char));
	memcpy(&capabilities, data, MIN(size, sizeof(capabilities)));
	g_variant_unref(params);

	hexdump("A2DP peer capabilities blob", &capabilities, size, true);
	if (a2dp_select_configuration(codec, &capabilities, size) == -1)
		goto fail;

	GVariant *rv[] = {
		g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, &capabilities, size, sizeof(uint8_t)) };
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(rv, 1));

	return;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Invalid capabilities");
}

static void bluez_endpoint_set_configuration(GDBusMethodInvocation *inv, void *userdata) {

	const char *sender = g_dbus_method_invocation_get_sender(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;
	const struct a2dp_codec *codec = dbus_obj->codec;
	const uint16_t codec_id = codec->codec_id;

	struct ba_adapter *a = NULL;
	struct ba_transport *t = NULL;
	struct ba_device *d = NULL;

	enum bluez_a2dp_transport_state state = 0xFFFF;
	char *device_path = NULL;
	a2dp_t configuration = {};
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

			if ((codec_id & 0xFF) != g_variant_get_byte(value)) {
				error("Invalid configuration: %s", "Codec mismatch");
				goto fail;
			}

		}
		else if (strcmp(property, "Configuration") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTESTRING, property)) {

			size_t size = 0;
			const void *data = g_variant_get_fixed_array(value, &size, sizeof(char));
			memcpy(&configuration, data, MIN(size, sizeof(configuration)));

			uint32_t rv;
			if ((rv = a2dp_check_configuration(codec, data, size)) != A2DP_CHECK_OK) {
				error("Invalid configuration: %s: %#x", "Invalid configuration blob", rv);
				goto fail;
			}

		}
		else if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
			state = bluez_a2dp_transport_state_from_string(g_variant_get_string(value, NULL));
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

	if (state == 0xFFFF) {
		error("Invalid configuration: %s", "Missing state");
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

	if (d->seps == NULL)
		d->seps = bluez_adapters_device_lookup(a->hci.dev_id, &addr);

	if (ba_transport_lookup(d, transport_path) != NULL) {
		error("Transport already configured: %s", transport_path);
		goto fail;
	}

	if ((t = ba_transport_new_a2dp(d, dbus_obj->profile,
					sender, transport_path, codec, &configuration)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	/* Skip volume level initialization in case of A2DP Source
	 * profile and software volume control. */
	if (!(t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE &&
				t->a2dp.pcm.soft_volume)) {

		int level = ba_transport_pcm_volume_bt_to_level(&t->a2dp.pcm, volume);

		pthread_mutex_lock(&t->a2dp.pcm.mutex);
		ba_transport_pcm_volume_set(&t->a2dp.pcm.volume[0], &level, NULL, NULL);
		ba_transport_pcm_volume_set(&t->a2dp.pcm.volume[1], &level, NULL, NULL);
		pthread_mutex_unlock(&t->a2dp.pcm.mutex);

	}

	t->a2dp.bluez_dbus_sep_path = dbus_obj->path;
	t->a2dp.delay = delay;
	t->a2dp.volume = volume;

	debug("%s configured for device %s",
			ba_transport_debug_name(t),
			batostr_(&d->addr));
	hexdump("A2DP selected configuration blob",
			&configuration, codec->capabilities_size, true);
	debug("PCM configuration: channels: %u, sampling: %u",
			t->a2dp.pcm.channels, t->a2dp.pcm.sampling);

	ba_transport_set_a2dp_state(t, state);
	dbus_obj->connected = true;

	g_dbus_method_invocation_return_value(inv, NULL);
	bluez_register_a2dp_all(a);
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
}

static void bluez_endpoint_clear_configuration(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;

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

	struct bluez_dbus_object_data *dbus_obj = userdata;

	debug("Releasing media endpoint: %s", dbus_obj->path);
	dbus_obj->connected = false;
	dbus_obj->registered = false;

	g_object_unref(inv);
}

/**
 * Register media endpoint in BlueZ. */
static int bluez_register_media_endpoint(
		const struct ba_adapter *adapter,
		const struct bluez_dbus_object_data *dbus_obj,
		const char *uuid,
		GError **error) {

	const struct a2dp_codec *codec = dbus_obj->codec;
	GDBusMessage *msg = NULL, *rep = NULL;
	int ret = 0;

	debug("Registering media endpoint: %s", dbus_obj->path);

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adapter->bluez_dbus_path,
			BLUEZ_IFACE_MEDIA, "RegisterEndpoint");

	GVariantBuilder properties;
	g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));

	g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(uuid));
	g_variant_builder_add(&properties, "{sv}", "DelayReporting", g_variant_new_boolean(TRUE));
	g_variant_builder_add(&properties, "{sv}", "Codec", g_variant_new_byte(codec->codec_id));
	g_variant_builder_add(&properties, "{sv}", "Capabilities", g_variant_new_fixed_array(
				G_VARIANT_TYPE_BYTE, &codec->capabilities, codec->capabilities_size, sizeof(uint8_t)));

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
		const struct a2dp_codec *codec,
		const char *uuid) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "SelectConfiguration",
			.handler = bluez_endpoint_select_configuration },
		{ .method = "SetConfiguration",
			.handler = bluez_endpoint_set_configuration },
		{ .method = "ClearConfiguration",
			.handler = bluez_endpoint_clear_configuration },
		{ .method = "Release",
			.handler = bluez_endpoint_release },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
	};

	enum ba_transport_profile profile = codec->dir == A2DP_SOURCE ?
			BA_TRANSPORT_PROFILE_A2DP_SOURCE : BA_TRANSPORT_PROFILE_A2DP_SINK;

	int registered = 0;
	int connected = 0;

	pthread_mutex_lock(&bluez_mutex);

	for (;;) {

		struct bluez_dbus_object_data *dbus_obj;
		GError *err = NULL;

		char path[sizeof(dbus_obj->path)];
		snprintf(path, sizeof(path), "/org/bluez/%s%s/%d", adapter->hci.name,
				bluez_transport_profile_to_bluez_object_path(profile, codec->codec_id),
				++registered);

		if ((dbus_obj = g_hash_table_lookup(dbus_object_data_map, path)) == NULL) {

			/* End the registration loop if all previously created media
			 * endpoints are registered in BlueZ and we've got at least N
			 * not connected endpoints. */
			if (registered > connected + 2)
				break;

			debug("Creating media endpoint object: %s", path);

			if ((dbus_obj = calloc(1, sizeof(*dbus_obj))) == NULL) {
				warn("Couldn't register media endpoint: %s", strerror(errno));
				goto fail;
			}

			strncpy(dbus_obj->path, path, sizeof(dbus_obj->path));
			dbus_obj->hci_dev_id = adapter->hci.dev_id;
			dbus_obj->codec = codec;
			dbus_obj->profile = profile;
			dbus_obj->ref_count = 2;

			bluez_MediaEndpointIfaceSkeleton *ifs_endpoint;
			if ((ifs_endpoint = bluez_media_endpoint_iface_skeleton_new(&vtable,
							dbus_obj, (GDestroyNotify)bluez_dbus_object_data_unref)) == NULL) {
				free(dbus_obj);
				goto fail;
			}

			dbus_obj->ifs = G_DBUS_INTERFACE_SKELETON(ifs_endpoint);
			if (!g_dbus_interface_skeleton_export(dbus_obj->ifs, config.dbus, path, &err)) {
				g_object_unref(ifs_endpoint);
				free(dbus_obj);
				goto fail;
			}

			g_hash_table_insert(dbus_object_data_map, dbus_obj->path, dbus_obj);

		}

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

	pthread_mutex_unlock(&bluez_mutex);

}

/**
 * Register A2DP endpoints. */
static void bluez_register_a2dp_all(struct ba_adapter *adapter) {

	struct a2dp_codec * const * cc = a2dp_codecs;
	for (const struct a2dp_codec *c = *cc; c != NULL; c = *++cc) {
		switch (c->dir) {
		case A2DP_SOURCE:
			if (config.profile.a2dp_source && c->enabled)
				bluez_register_a2dp(adapter, c, BLUETOOTH_UUID_A2DP_SOURCE);
			break;
		case A2DP_SINK:
			if (config.profile.a2dp_sink && c->enabled)
				bluez_register_a2dp(adapter, c, BLUETOOTH_UUID_A2DP_SINK);
			break;
		}
	}

}

static GVariant *bluez_battery_provider_iface_skeleton_get_properties(
		void *userdata) {

	const struct ba_device *d = userdata;

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	g_variant_builder_add(&props, "{sv}", "Device", ba_variant_new_device_path(d));
	g_variant_builder_add(&props, "{sv}", "Percentage", ba_variant_new_device_battery(d));
	g_variant_builder_add(&props, "{sv}", "Source", g_variant_new_string("BlueALSA"));

	return g_variant_builder_end(&props);
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
static bool bluez_battery_provider_manager_add(struct ba_device *device) {

	static const GDBusInterfaceSkeletonVTable vtable = {
		.get_properties = bluez_battery_provider_iface_skeleton_get_properties,
		.get_property = bluez_battery_provider_iface_skeleton_get_property,
	};

	struct ba_adapter *a = device->a;
	GDBusObjectManagerServer *manager = bluez_adapters[a->hci.dev_id].battery_manager;

	if (device->ba_battery_dbus_path != NULL)
		return true;

	GDBusObjectSkeleton *skeleton = NULL;
	bluez_BatteryProviderIfaceSkeleton *ifs_battery_provider = NULL;

	char *path = g_strdup_printf("/org/bluez/%s/battery/%s",
			a->hci.name, device->addr_dbus_str);
	if ((skeleton = g_dbus_object_skeleton_new(path)) == NULL)
		goto fail;
	if ((ifs_battery_provider = bluez_battery_provider_iface_skeleton_new(&vtable,
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
static bool bluez_battery_provider_manager_remove(struct ba_device *device) {

	struct ba_adapter *a = device->a;
	GDBusObjectManagerServer *manager = bluez_adapters[a->hci.dev_id].battery_manager;

	if (device->ba_battery_dbus_path == NULL)
		return true;

	char *path = device->ba_battery_dbus_path;
	device->ba_battery_dbus_path = NULL;

	debug("Removing battery from battery provider: %s", path);
	g_dbus_object_manager_server_unexport(manager, path);
	g_free(path);

	return true;
}

/**
 * Register battery provider in BlueZ. */
static void bluez_register_battery_provider_manager(struct bluez_adapter *b_adapter) {

	struct ba_adapter *a = b_adapter->adapter;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;

	char path[64];
	snprintf(path, sizeof(path), "/org/bluez/%s/battery", a->hci.name);

	debug("Registering battery provider: %s", path);

	GDBusObjectManagerServer *manager = g_dbus_object_manager_server_new(path);
	g_dbus_object_manager_server_set_connection(manager, config.dbus);
	b_adapter->battery_manager = manager;

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, a->bluez_dbus_path,
			BLUEZ_IFACE_BATTERY_PROVIDER_MANAGER, "RegisterBatteryProvider");

	g_dbus_message_set_body(msg, g_variant_new("(o)", path));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		if (err->code == G_DBUS_ERROR_UNKNOWN_METHOD) {
			/* Suppress warning message in case when BlueZ has no battery provider
			 * support enabled, because it's not a mandatory feature. */
			debug("BlueZ battery provider support not available");
			g_error_free(err);
			err = NULL;
		}
		goto fail;
	}

fail:
	if (err != NULL) {
		warn("Couldn't register battery provider: %s", err->message);
		g_error_free(err);
	}
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
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

	if ((t = ba_transport_new_sco(d, dbus_obj->profile,
					sender, device_path, fd)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	if (sco_setup_connection_dispatcher(a) == -1) {
		error("Couldn't setup SCO connection dispatcher: %s", strerror(errno));
		goto fail;
	}

	debug("%s configured for device %s",
			ba_transport_debug_name(t),
			batostr_(&d->addr));

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
 * Register hands-free profile in BlueZ. */
static int bluez_register_profile(
		const struct bluez_dbus_object_data *dbus_obj,
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
		enum ba_transport_profile profile,
		uint16_t version,
		uint16_t features) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "NewConnection",
			.handler = bluez_profile_new_connection },
		{ .method = "RequestDisconnection",
			.handler = bluez_profile_request_disconnection },
		{ .method = "Release",
			.handler = bluez_profile_release },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
	};

	pthread_mutex_lock(&bluez_mutex);

	struct bluez_dbus_object_data *dbus_obj;
	GError *err = NULL;

	char path[sizeof(dbus_obj->path)];
	snprintf(path, sizeof(path), "/org/bluez%s",
			bluez_transport_profile_to_bluez_object_path(profile, -1));

	if ((dbus_obj = g_hash_table_lookup(dbus_object_data_map, path)) == NULL) {

		debug("Creating hands-free profile object: %s", path);

		if ((dbus_obj = calloc(1, sizeof(*dbus_obj))) == NULL) {
			warn("Couldn't register hands-free profile: %s", strerror(errno));
			goto fail;
		}

		strncpy(dbus_obj->path, path, sizeof(dbus_obj->path));
		dbus_obj->hci_dev_id = -1;
		dbus_obj->profile = profile;
		dbus_obj->ref_count = 2;

		bluez_ProfileIfaceSkeleton *ifs_profile;
		if ((ifs_profile = bluez_profile_iface_skeleton_new(&vtable,
						dbus_obj, (GDestroyNotify)bluez_dbus_object_data_unref)) == NULL) {
			free(dbus_obj);
			goto fail;
		}

		dbus_obj->ifs = G_DBUS_INTERFACE_SKELETON(ifs_profile);
		if (!g_dbus_interface_skeleton_export(dbus_obj->ifs, config.dbus, path, &err)) {
			g_object_unref(ifs_profile);
			free(dbus_obj);
			goto fail;
		}

		g_hash_table_insert(dbus_object_data_map, dbus_obj->path, dbus_obj);

	}

	if (!dbus_obj->registered) {
		if (bluez_register_profile(dbus_obj, uuid, version, features, &err) == -1)
			goto fail;
		dbus_obj->registered = true;
	}

fail:

	if (err != NULL) {
		warn("Couldn't register hands-free profile: %s", err->message);
		g_error_free(err);
	}

	pthread_mutex_unlock(&bluez_mutex);

}

/**
 * Register Bluetooth Hands-Free Audio Profiles.
 *
 * This function also registers deprecated HSP profile. Profiles registration
 * is controlled by the global configuration structure - if none is enabled,
 * this function will do nothing. */
static void bluez_register_hfp_all(void) {
	if (config.profile.hsp_hs)
		bluez_register_hfp(BLUETOOTH_UUID_HSP_HS, BA_TRANSPORT_PROFILE_HSP_HS,
				0x0102 /* HSP 1.2 */, 0x1 /* remote audio volume control */);
	if (config.profile.hsp_ag)
		bluez_register_hfp(BLUETOOTH_UUID_HSP_AG, BA_TRANSPORT_PROFILE_HSP_AG,
				0x0102 /* HSP 1.2 */, 0x0);
	if (config.profile.hfp_hf)
		bluez_register_hfp(BLUETOOTH_UUID_HFP_HF, BA_TRANSPORT_PROFILE_HFP_HF,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_hf);
	if (config.profile.hfp_ag)
		bluez_register_hfp(BLUETOOTH_UUID_HFP_AG, BA_TRANSPORT_PROFILE_HFP_AG,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_ag);
}

/**
 * Register to the BlueZ service. */
static void bluez_register(void) {

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
			bluez_adapter_new(a);
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
	(void)interface_;
	(void)signal;
	(void)userdata;

	GVariantIter *interfaces;
	GVariantIter *properties;
	GVariant *value;
	const char *object_path;
	const char *interface;
	const char *property;

	int hci_dev_id = -1;
	struct a2dp_sep sep = {
		.dir = A2DP_SOURCE,
		.codec_id = 0xFFFF,
	};

	g_variant_get(params, "(&oa{sa{sv}})", &object_path, &interfaces);
	while (g_variant_iter_next(interfaces, "{&sa{sv}}", &interface, &properties)) {
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0) {
			while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
				if (strcmp(property, "Address") == 0 &&
						/* make sure that this new BT adapter matches our HCI filter */
						bluez_match_dbus_adapter(object_path, g_variant_get_string(value, NULL)))
					hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);
				g_variant_unref(value);
			}
		}
		else if (strcmp(interface, BLUEZ_IFACE_MEDIA_ENDPOINT) == 0) {

			/* Check whether this new media endpoint interface was added in the HCI
			 * which exists in our local BlueZ adapter cache - HCI that matches our
			 * HCI filter. */
			int dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);
			if (dev_id == -1 || bluez_adapters[dev_id].adapter == NULL)
				continue;

			while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
				if (strcmp(property, "UUID") == 0) {
					const char *uuid = g_variant_get_string(value, NULL);
					if (strcasecmp(uuid, BLUETOOTH_UUID_A2DP_SINK) == 0)
						sep.dir = A2DP_SINK;
				}
				else if (strcmp(property, "Codec") == 0)
					sep.codec_id = g_variant_get_byte(value);
				else if (strcmp(property, "Capabilities") == 0) {

					const void *data = g_variant_get_fixed_array(value,
							&sep.capabilities_size, sizeof(char));

					if (sep.capabilities_size > sizeof(sep.capabilities)) {
						warn("Capabilities blob size exceeded: %zu > %zu",
								sep.capabilities_size, sizeof(sep.capabilities));
						sep.capabilities_size = sizeof(sep.capabilities);
					}

					memcpy(&sep.capabilities, data, sep.capabilities_size);

				}
				g_variant_unref(value);

			}
		}
		g_variant_iter_free(properties);
	}
	g_variant_iter_free(interfaces);

	struct ba_adapter *a;
	if (hci_dev_id != -1 &&
			(a = ba_adapter_new(hci_dev_id)) != NULL) {
		bluez_adapter_new(a);
	}

	/* HFP has to be registered globally */
	if (strcmp(object_path, "/org/bluez") == 0)
		bluez_register_hfp_all();

	if (sep.codec_id != 0xFFFF) {

		bdaddr_t addr;
		g_dbus_bluez_object_path_to_bdaddr(object_path, &addr);
		int dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);

		GArray *seps;
		if ((seps = bluez_adapters_device_lookup(dev_id, &addr)) == NULL)
			g_hash_table_insert(bluez_adapters[dev_id].device_sep_map,
					g_memdup2(&addr, sizeof(addr)), seps = g_array_new(FALSE, FALSE, sizeof(sep)));

		strncpy(sep.bluez_dbus_path, object_path, sizeof(sep.bluez_dbus_path) - 1);
		if (sep.codec_id == A2DP_CODEC_VENDOR)
			sep.codec_id = a2dp_get_vendor_codec_id(&sep.capabilities, sep.capabilities_size);

		debug("Adding new Stream End-Point: %s: %s", batostr_(&addr),
				a2dp_codecs_codec_id_to_string(sep.codec_id));
		g_array_append_val(seps, sep);

		/* Collected SEPs are exposed via BlueALSA D-Bus API. We will sort them
		 * here, so the D-Bus API will return codecs in the defined order. */
		g_array_sort(seps, (GCompareFunc)a2dp_sep_cmp);

	}

}

static void bluez_signal_interfaces_removed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface_, signal);
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
			while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj))
				if (dbus_obj->hci_dev_id == hci_dev_id) {
					g_dbus_interface_skeleton_unexport(dbus_obj->ifs);
					g_object_unref(dbus_obj->ifs);
					g_hash_table_iter_remove(&iter);
				}

			bluez_adapter_free(&bluez_adapters[hci_dev_id]);

		}
		else if (strcmp(interface, BLUEZ_IFACE_MEDIA_ENDPOINT) == 0) {

			GArray *seps;
			bdaddr_t addr;
			size_t i;

			g_dbus_bluez_object_path_to_bdaddr(object_path, &addr);
			if ((seps = bluez_adapters_device_lookup(hci_dev_id, &addr)) != NULL)
				for (i = 0; i < seps->len; i++)
					if (strcmp(bluez_adapters_device_get_sep(seps, i).bluez_dbus_path, object_path) == 0)
						g_array_remove_index_fast(seps, i);

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
		debug("Signal: %s.%s(): %s: %s", interface_, signal, interface, property);

		if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
			const char *state = g_variant_get_string(value, NULL);
			ba_transport_set_a2dp_state(t, bluez_a2dp_transport_state_from_string(state));
		}
		else if (strcmp(property, "Delay") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			t->a2dp.delay = g_variant_get_uint16(value);
			bluealsa_dbus_pcm_update(&t->a2dp.pcm, BA_DBUS_PCM_UPDATE_DELAY);
		}
		else if (strcmp(property, "Volume") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			/* received volume is in range [0, 127] */
			uint16_t volume = t->a2dp.volume = g_variant_get_uint16(value);
			if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE &&
					t->a2dp.pcm.soft_volume)
				debug("Skipping A2DP volume update: %u", volume);
			else {

				int level = ba_transport_pcm_volume_bt_to_level(&t->a2dp.pcm, volume);
				debug("Updating A2DP volume: %u [%.2f dB]", volume, 0.01 * level);

				pthread_mutex_lock(&t->a2dp.pcm.mutex);
				ba_transport_pcm_volume_set(&t->a2dp.pcm.volume[0], &level, NULL, NULL);
				ba_transport_pcm_volume_set(&t->a2dp.pcm.volume[1], &level, NULL, NULL);
				pthread_mutex_unlock(&t->a2dp.pcm.mutex);

				bluealsa_dbus_pcm_update(&t->a2dp.pcm, BA_DBUS_PCM_UPDATE_VOLUME);

			}
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

	GHashTableIter iter;
	struct bluez_dbus_object_data *dbus_obj;
	g_hash_table_iter_init(&iter, dbus_object_data_map);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj)) {
		g_dbus_interface_skeleton_unexport(dbus_obj->ifs);
		g_object_unref(dbus_obj->ifs);
		g_hash_table_iter_remove(&iter);
	}

	size_t i;
	for (i = 0; i < ARRAYSIZE(bluez_adapters); i++)
		bluez_adapter_free(&bluez_adapters[i]);

	pthread_mutex_unlock(&bluez_mutex);

}

/**
 * Subscribe to BlueZ signals. */
static void bluez_subscribe_signals(void) {

	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			DBUS_IFACE_OBJECT_MANAGER, "InterfacesAdded", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_interfaces_added, NULL, NULL);
	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			DBUS_IFACE_OBJECT_MANAGER, "InterfacesRemoved", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_interfaces_removed, NULL, NULL);

	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			DBUS_IFACE_PROPERTIES, "PropertiesChanged", NULL, BLUEZ_IFACE_MEDIA_TRANSPORT,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_transport_changed, NULL, NULL);

	g_bus_watch_name_on_connection(config.dbus, BLUEZ_SERVICE,
			G_BUS_NAME_WATCHER_FLAGS_NONE, NULL, bluez_disappeared,
			NULL, NULL);

}

/**
 * Initialize integration with BlueZ service.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_init(void) {

	if (dbus_object_data_map == NULL)
		dbus_object_data_map = g_hash_table_new_full(g_str_hash, g_str_equal,
				NULL, (GDestroyNotify)bluez_dbus_object_data_unref);

	bluez_subscribe_signals();
	bluez_register();

	return 0;
}

/**
 * Set new configuration for already connected A2DP endpoint.
 *
 * @param dbus_current_sep_path D-Bus SEP path of current connection.
 * @param sep New SEP to be configured.
 * @param error NULL GError pointer.
 * @return On success this function returns true. */
bool bluez_a2dp_set_configuration(
		const char *dbus_current_sep_path,
		const struct a2dp_sep *sep,
		GError **error) {

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(sep->bluez_dbus_path);
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
				dbus_obj->codec->codec_id == sep->codec_id &&
				dbus_obj->codec->dir == !sep->dir &&
				dbus_obj->registered) {

			/* reuse already selected endpoint path */
			if (strcmp(dbus_obj->path, dbus_current_sep_path) == 0) {
				endpoint = dbus_obj->path;
				break;
			}

			if (!dbus_obj->connected)
				endpoint = dbus_obj->path;

		}

	if (endpoint == NULL) {
		pthread_mutex_unlock(&bluez_mutex);
		*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
				"Extra A2DP endpoint not available");
		goto fail;
	}

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&props, "{sv}", "Capabilities", g_variant_new_fixed_array(
				G_VARIANT_TYPE_BYTE, &sep->configuration, sep->capabilities_size, sizeof(uint8_t)));

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE,
			sep->bluez_dbus_path, BLUEZ_IFACE_MEDIA_ENDPOINT, "SetConfiguration");
	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", endpoint, &props));
	g_variant_builder_clear(&props);

	pthread_mutex_unlock(&bluez_mutex);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

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
		bluez_battery_provider_manager_remove(device);
		return;
	}

	if (!bluez_battery_provider_manager_add(device))
		return;

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	g_variant_builder_add(&props, "{sv}", "Percentage",
			ba_variant_new_device_battery(device));

	g_dbus_connection_emit_properties_changed(config.dbus,
			device->ba_battery_dbus_path, BLUEZ_IFACE_BATTERY_PROVIDER, &props, NULL);
	g_variant_builder_clear(&props);

}
