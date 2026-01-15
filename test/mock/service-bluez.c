/*
 * service-bluez.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "bluez-iface.h"
#include "shared/a2dp-codecs.h"
#include "shared/bluetooth.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "utils.h"

#include "dbus-ifaces.h"

typedef struct BlueZMockServicePriv {
	/* Public members. */
	BlueZMockService p;
	/* Global BlueZ mock server object manager. */
	GDBusObjectManagerServer * server;
	/* Client manager for registered media application. */
	GDBusObjectManager * media_app_client;
	/* Mapping between profile UUID and its proxy object. */
	GHashTable * profiles;
	/* Registered GATT application (service + characteristic). */
	MockBluezGattService1 * gatt_service;
	MockBluezGattCharacteristic1 * gatt_characteristic;
	/* Registered LE advertisement. */
	MockBluezLEAdvertisement1 * advertisement;
} BlueZMockServicePriv;

/* Bluetooth device name mappings in form of "MAC:name". */
static const char * devices[8] = { NULL };

int mock_bluez_service_add_device_name_mapping(const char * mapping) {
	for (size_t i = 0; i < ARRAYSIZE(devices); i++)
		if (devices[i] == NULL) {
			devices[i] = strdup(mapping);
			return 0;
		}
	return -1;
}

typedef struct {
	BlueZMockServicePriv * service;
	char * uuid;
} RegisterProfileData;

static void mock_bluez_profile_proxy_finish(G_GNUC_UNUSED GObject * source,
		GAsyncResult * result, void * userdata) {
	g_autofree RegisterProfileData * data = userdata;
	BlueZMockServicePriv * self = data->service;

	MockBluezProfile1 * profile = mock_bluez_profile1_proxy_new_finish(result, NULL);
	g_hash_table_insert(self->profiles, g_steal_pointer(&data->uuid), profile);
	/* Add profile to the ready queue. */
	g_async_queue_push(self->p.profile_ready_queue, profile);
}

static gboolean mock_bluez_register_profile_handler(MockBluezProfileManager1 *manager,
		GDBusMethodInvocation *invocation, const char *path, const char *uuid,
		G_GNUC_UNUSED GVariant * options, void * userdata) {
	BlueZMockServicePriv * self = userdata;

	RegisterProfileData * data = g_new0(RegisterProfileData, 1);
	data->uuid = g_strdup(uuid);
	data->service = self;

	GDBusConnection * conn = g_dbus_method_invocation_get_connection(invocation);
	const char * sender = g_dbus_method_invocation_get_sender(invocation);
	mock_bluez_profile1_proxy_new(conn, G_DBUS_PROXY_FLAGS_NONE, sender, path,
			NULL, mock_bluez_profile_proxy_finish, data);

	mock_bluez_profile_manager1_complete_register_profile(manager, invocation);
	return TRUE;
}

static void mock_bluez_add_profile_manager(BlueZMockServicePriv * self, const char * path) {

	g_autoptr(MockBluezProfileManager1) manager = mock_bluez_profile_manager1_skeleton_new();
	g_signal_connect(manager, "handle-register-profile",
			G_CALLBACK(mock_bluez_register_profile_handler), self);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(manager));
	g_dbus_object_manager_server_export(self->server, skeleton);

}

static gboolean bat_manager_register_provider_handler(MockBluezBatteryProviderManager1 * obj,
		GDBusMethodInvocation * invocation, G_GNUC_UNUSED const char * path,
		G_GNUC_UNUSED GVariant * options, G_GNUC_UNUSED void * userdata) {
	mock_bluez_battery_provider_manager1_complete_register_battery_provider(obj, invocation);
	return TRUE;
}

static gboolean gatt_manager_register_application_handler(MockBluezGattManager1 * obj,
		GDBusMethodInvocation * invocation, G_GNUC_UNUSED const char * path,
		G_GNUC_UNUSED GVariant * options, void * userdata) {
	BlueZMockServicePriv * self = userdata;

	g_autoptr(GError) err = NULL;
	g_autoptr(GDBusObjectManager) client;
	GDBusConnection * conn = g_dbus_method_invocation_get_connection(invocation);
	const char * sender = g_dbus_method_invocation_get_sender(invocation);
	if ((client = mock_object_manager_client_new_sync(conn,
					G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, sender, path, NULL, &err)) == NULL) {
		error("Failed to create GATT application client: %s", err->message);
		g_dbus_method_invocation_return_error_literal(invocation,
				G_DBUS_ERROR, G_DBUS_ERROR_FAILED, err->message);
		return TRUE;
	}

	void * object;
	/* Retrieve GATT service and characteristic objects from the application. */
	g_autoptr(GObjectList) objects = g_dbus_object_manager_get_objects(client);
	for (GList * elem = objects; elem != NULL; elem = elem->next) {
		if ((object = mock_object_get_bluez_gatt_service1(elem->data)) != NULL)
			self->gatt_service = object;
		else if ((object = mock_object_get_bluez_gatt_characteristic1(elem->data)) != NULL)
			self->gatt_characteristic = object;
	}

	mock_bluez_gatt_manager1_complete_register_application(obj, invocation);
	return TRUE;
}

char * mock_bluez_service_get_gatt_service_uuid(BlueZMockService * srv) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;
	if (self->gatt_service == NULL)
		return NULL;
	return mock_bluez_gatt_service1_dup_uuid(self->gatt_service);
}

char * mock_bluez_service_get_gatt_characteristic_uuid(BlueZMockService * srv) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;
	if (self->gatt_characteristic == NULL)
		return NULL;
	return mock_bluez_gatt_characteristic1_dup_uuid(self->gatt_characteristic);
}

GVariant * mock_bluez_service_get_gatt_characteristic_value(BlueZMockService * srv) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;
	if (self->gatt_characteristic == NULL)
		return NULL;
	g_autoptr(GVariant) value = NULL;
	mock_bluez_gatt_characteristic1_call_read_value_sync(self->gatt_characteristic,
			g_variant_new("a{sv}", NULL), &value, NULL, NULL);
	return g_steal_pointer(&value);
}

GIOChannel * mock_bluez_service_acquire_gatt_characteristic_notify_channel(
		BlueZMockService * srv) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;
  g_autoptr(GUnixFDList) fd_list = NULL;
	mock_bluez_gatt_characteristic1_call_acquire_notify_sync(self->gatt_characteristic,
			g_variant_new("a{sv}", NULL), NULL, NULL, NULL, &fd_list, NULL, NULL);
	/* Wrap the acquired file descriptor in an IO channel. */
	return g_io_channel_unix_raw_new(g_unix_fd_list_get(fd_list, 0, NULL));
}

GIOChannel * mock_bluez_service_acquire_gatt_characteristic_write_channel(
		BlueZMockService * srv) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;
	g_autoptr(GUnixFDList) fd_list = NULL;
	mock_bluez_gatt_characteristic1_call_acquire_write_sync(self->gatt_characteristic,
			g_variant_new("a{sv}", NULL), NULL, NULL, NULL, &fd_list, NULL, NULL);
	/* Wrap the acquired file descriptor in an IO channel. */
	return g_io_channel_unix_raw_new(g_unix_fd_list_get(fd_list, 0, NULL));
}

static gboolean adv_manager_handle_register_advertisement(MockBluezLEAdvertisingManager1 * obj,
		GDBusMethodInvocation * invocation, const char * path,
		G_GNUC_UNUSED GVariant * options, void * userdata) {
	BlueZMockServicePriv * self = userdata;

	g_autoptr(GError) err = NULL;
	GDBusConnection * conn = g_dbus_method_invocation_get_connection(invocation);
	const char * sender = g_dbus_method_invocation_get_sender(invocation);
	if ((self->advertisement = mock_bluez_leadvertisement1_proxy_new_sync(conn,
					G_DBUS_PROXY_FLAGS_NONE, sender, path, NULL, &err)) == NULL) {
		error("Failed to create LE advertisement proxy: %s", err->message);
		g_dbus_method_invocation_return_error_literal(invocation,
				G_DBUS_ERROR, G_DBUS_ERROR_FAILED, err->message);
		return TRUE;
	}

	mock_bluez_leadvertising_manager1_complete_register_advertisement(obj, invocation);
	return TRUE;
}

char * mock_bluez_service_get_advertisement_name(BlueZMockService * srv) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;
	if (self->advertisement == NULL)
		return NULL;
	return mock_bluez_leadvertisement1_dup_local_name(self->advertisement);
}

GVariant * mock_bluez_service_get_advertisement_service_data(BlueZMockService * srv,
		const char * uuid) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;
	if (self->advertisement == NULL)
		return NULL;
	g_autoptr(GVariant) data = mock_bluez_leadvertisement1_dup_service_data(self->advertisement);
	return g_variant_lookup_value(data, uuid, G_VARIANT_TYPE_BYTESTRING);
}

static gboolean adv_manager_handle_unregister_advertisement(MockBluezLEAdvertisingManager1 * obj,
		GDBusMethodInvocation * invocation, G_GNUC_UNUSED const char * path,
		G_GNUC_UNUSED void * userdata) {
	mock_bluez_leadvertising_manager1_complete_unregister_advertisement(obj, invocation);
	return TRUE;
}

static void mock_bluez_media_application_client_finish(G_GNUC_UNUSED GObject * source,
		GAsyncResult * result, void * userdata) {
	BlueZMockServicePriv * self = userdata;
	self->media_app_client = mock_object_manager_client_new_finish(result, NULL);
	/* Add media application to the ready queue. */
	g_async_queue_push(self->p.media_application_ready_queue, self->media_app_client);
}

static gboolean media_register_application_handler(MockBluezMedia1 * obj,
		GDBusMethodInvocation * invocation, const char * path,
		G_GNUC_UNUSED GVariant * options, void * userdata) {
	BlueZMockServicePriv * self = userdata;

	GDBusConnection * conn = g_dbus_method_invocation_get_connection(invocation);
	const char * sender = g_dbus_method_invocation_get_sender(invocation);
	mock_object_manager_client_new(conn, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
			sender, path, NULL, mock_bluez_media_application_client_finish, self);

	mock_bluez_media1_complete_register_application(obj, invocation);
	return TRUE;
}

static void mock_bluez_add_adapter(BlueZMockServicePriv * self,
		const char * adapter_path, const char * address) {

	g_autoptr(MockBluezAdapter1) adapter = mock_bluez_adapter1_skeleton_new();
	mock_bluez_adapter1_set_address(adapter, address);

	g_autoptr(MockBluezBatteryProviderManager1) bat = mock_bluez_battery_provider_manager1_skeleton_new();
	g_signal_connect(bat, "handle-register-battery-provider",
			G_CALLBACK(bat_manager_register_provider_handler), self);

	g_autoptr(MockBluezGattManager1) gatt = mock_bluez_gatt_manager1_skeleton_new();
	g_signal_connect(gatt, "handle-register-application",
			G_CALLBACK(gatt_manager_register_application_handler), self);

	g_autoptr(MockBluezLEAdvertisingManager1) adv = mock_bluez_leadvertising_manager1_skeleton_new();
	g_signal_connect(adv, "handle-register-advertisement",
			G_CALLBACK(adv_manager_handle_register_advertisement), self);
	g_signal_connect(adv, "handle-unregister-advertisement",
			G_CALLBACK(adv_manager_handle_unregister_advertisement), self);

	g_autoptr(MockBluezMedia1) media = mock_bluez_media1_skeleton_new();
	g_signal_connect(media, "handle-register-application",
			G_CALLBACK(media_register_application_handler), self);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(adapter_path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(adapter));
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(bat));
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(gatt));
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(adv));
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(media));
	g_dbus_object_manager_server_export(self->server, skeleton);

}

static void mock_bluez_adapter_add_device(BlueZMockServicePriv * self,
		const char * adapter_path, const char * device_path, const char * address) {

	g_autoptr(MockBluezDevice1) device = mock_bluez_device1_skeleton_new();
	mock_bluez_device1_set_adapter(device, adapter_path);
	mock_bluez_device1_set_alias(device, address);
	mock_bluez_device1_set_icon(device, "audio-card");
	mock_bluez_device1_set_trusted(device, TRUE);

	for (size_t i = 0; i < ARRAYSIZE(devices); i++)
		if (devices[i] != NULL &&
				strncmp(devices[i], address, strlen(address)) == 0)
			mock_bluez_device1_set_alias(device, &devices[i][strlen(address) + 1]);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(device_path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(device));
	g_dbus_object_manager_server_export(self->server, skeleton);

}

static gboolean mock_bluez_media_ep_set_configuration_handler(MockBluezMediaEndpoint1 *endpoint,
		GDBusMethodInvocation *invocation, G_GNUC_UNUSED const char *transport,
		G_GNUC_UNUSED GVariant *props, G_GNUC_UNUSED void *userdata) {
	mock_bluez_media_endpoint1_complete_set_configuration(endpoint, invocation);
	return TRUE;
}

void mock_bluez_service_device_add_media_endpoint(BlueZMockService * srv,
		const char * device_path, const char * endpoint_path, const char * uuid,
		uint32_t codec_id, const void * capabilities, size_t capabilities_size) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;

	g_autoptr(MockBluezMediaEndpoint1) endpoint = mock_bluez_media_endpoint1_skeleton_new();
	mock_bluez_media_endpoint1_set_uuid(endpoint, uuid);
	mock_bluez_media_endpoint1_set_codec(endpoint, codec_id);
	mock_bluez_media_endpoint1_set_capabilities(endpoint,
				g_variant_new_fixed_byte_array(capabilities, capabilities_size));
	mock_bluez_media_endpoint1_set_device(endpoint, device_path);

	g_signal_connect(endpoint, "handle-set-configuration",
			G_CALLBACK(mock_bluez_media_ep_set_configuration_handler), self);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(endpoint_path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(endpoint));
	g_dbus_object_manager_server_export(self->server, skeleton);

}

static gboolean media_transport_acquire_handler(MockBluezMediaTransport1 * transport,
		GDBusMethodInvocation * invocation, G_GNUC_UNUSED void * userdata) {

	int fds[2];
	g_assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);

	g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new_from_array(&fds[0], 1);
	mock_bluez_media_transport1_complete_try_acquire(transport, invocation,
			fd_list, g_variant_new_handle(0), 256, 256);

	g_autoptr(GIOChannel) ch = g_io_channel_unix_raw_new(fds[1]);
	g_autoptr(GSource) watch = g_io_create_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR);
	g_source_set_callback(watch, G_SOURCE_FUNC(channel_drain_callback), NULL, NULL);
	g_source_attach(watch, NULL);

	mock_bluez_media_transport1_set_state(transport, "active");

	return TRUE;
}

static gboolean media_transport_release_handler(MockBluezMediaTransport1 *transport,
		GDBusMethodInvocation *invocation, G_GNUC_UNUSED void *userdata) {
	mock_bluez_media_transport1_complete_release(transport, invocation);
	mock_bluez_media_transport1_set_state(transport, "idle");
	return TRUE;
}

static MockBluezMediaTransport1 * mock_bluez_device_add_media_transport(
		BlueZMockServicePriv * self, const char * device_path, const char * transport_path) {

	MockBluezMediaTransport1 * transport = mock_bluez_media_transport1_skeleton_new();
	mock_bluez_media_transport1_set_device(transport, device_path);
	mock_bluez_media_transport1_set_state(transport, "idle");

	g_signal_connect(transport, "handle-acquire",
			G_CALLBACK(media_transport_acquire_handler), self);
	g_signal_connect(transport, "handle-try-acquire",
			G_CALLBACK(media_transport_acquire_handler), self);
	g_signal_connect(transport, "handle-release",
			G_CALLBACK(media_transport_release_handler), self);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(transport_path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(transport));
	g_dbus_object_manager_server_export(self->server, skeleton);

	return transport;
}

static int profile_rfcomm_callback(GIOChannel * ch,
		G_GNUC_UNUSED GIOCondition cond, G_GNUC_UNUSED void * userdata) {

	static const struct {
		const char * command;
		const char * response;
	} responses[] = {
		/* Accept HFP codec selection. */
		{ "\r\n+BCS:1\r\n", "AT+BCS=1\r" },
		{ "\r\n+BCS:2\r\n", "AT+BCS=2\r" },
		{ "\r\n+BCS:3\r\n", "AT+BCS=3\r" },
		/* Reply to HF query for supported features. */
		{ "AT+BRSF=756\r", "\r\n+BRSF=4095\r\n\r\nOK\r\n" },
		/* Reply to speaker/mic gain initial setup. */
		{ "AT+VGM=15\r", "\r\nOK\r\n" },
		{ "AT+VGS=15\r", "\r\nOK\r\n" },
	};

	char buffer[1024];
	size_t len;

	g_autoptr(GError) err = NULL;
	switch (g_io_channel_read_chars(ch, buffer, sizeof(buffer), &len, &err)) {
	case G_IO_STATUS_AGAIN:
		return G_SOURCE_CONTINUE;
	case G_IO_STATUS_ERROR:
		error("RFCOMM channel read error: %s", err->message);
		return G_SOURCE_CONTINUE;
	case G_IO_STATUS_EOF:
		return G_SOURCE_REMOVE;
	case G_IO_STATUS_NORMAL:
		hexdump("RFCOMM", buffer, len);

		const char * response = "\r\nERROR\r\n";
		for (size_t i = 0; i < ARRAYSIZE(responses); i++) {
			if (strncmp(buffer, responses[i].command, len) != 0)
				continue;
			response = responses[i].response;
			break;
		}

		if (g_io_channel_write_chars(ch, response, -1, &len, &err) != G_IO_STATUS_NORMAL)
			warn("Couldn't write RFCOMM response: %s", err->message);

	}

	return G_SOURCE_CONTINUE;
}

static void profile_new_connection_finish(GObject * source, GAsyncResult * result,
		void * userdata) {
	mock_bluez_profile1_call_new_connection_finish(MOCK_BLUEZ_PROFILE1(source), NULL, result, NULL);
	/* Notify the caller that the connection is ready. */
	g_async_queue_push(userdata, GINT_TO_POINTER(1));
}

void mock_bluez_service_device_profile_new_connection(BlueZMockService * srv,
		const char * device_path, const char * uuid, GAsyncQueue * ready) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;

	int fds[2];
	g_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

	g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new_from_array(&fds[0], 1);
	mock_bluez_profile1_call_new_connection(g_hash_table_lookup(self->profiles, uuid),
			device_path, g_variant_new_handle(0), g_variant_new("a{sv}", NULL),
			fd_list, NULL, profile_new_connection_finish, ready);

	g_autoptr(GIOChannel) ch = g_io_channel_unix_raw_new(fds[1]);
	g_autoptr(GSource) watch = g_io_create_watch(ch, G_IO_IN);
	g_source_set_callback(watch, G_SOURCE_FUNC(profile_rfcomm_callback), self, NULL);
	g_source_attach(watch, NULL);

}

static int media_transport_update(MockBluezMediaTransport1 * transport) {
	/* Pseudo-random hash based on the device path to simulate different values. */
	unsigned int hash = g_str_hash(mock_bluez_media_transport1_get_device(transport));
	mock_bluez_media_transport1_set_delay(transport, hash % 2777);
	mock_bluez_media_transport1_set_volume(transport, hash % 101);
	return G_SOURCE_REMOVE;
}

static void mock_bluez_media_endpoint_set_configuration_finish(GObject * source,
		GAsyncResult * result, void * userdata) {
	MockBluezMediaEndpoint1 * endpoint = MOCK_BLUEZ_MEDIA_ENDPOINT1(source);
	mock_bluez_media_endpoint1_call_set_configuration_finish(endpoint, result, NULL);
	/* Notify the caller that the configuration is done. */
	g_async_queue_push(userdata, GINT_TO_POINTER(1));
}

void mock_bluez_service_device_media_set_configuration(BlueZMockService * srv,
		const char * device_path, const char * transport_path, const char * uuid,
		uint32_t codec_id, const void * configuration, size_t configuration_size,
		GAsyncQueue * ready) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;

	const uint8_t codec = codec_id < A2DP_CODEC_VENDOR ? codec_id : A2DP_CODEC_VENDOR;
	const uint32_t vendor = codec_id < A2DP_CODEC_VENDOR ? 0 : codec_id;
	bool found = false;

	g_autoptr(GObjectList) endpoints = g_dbus_object_manager_get_objects(self->media_app_client);
	for (GList * elem = endpoints; elem != NULL; elem = elem->next) {
		MockBluezMediaEndpoint1 * ep = mock_object_peek_bluez_media_endpoint1(elem->data);
		if (mock_bluez_media_endpoint1_get_device(ep) == NULL &&
				strcmp(mock_bluez_media_endpoint1_get_uuid(ep), uuid) == 0 &&
				mock_bluez_media_endpoint1_get_codec(ep) == codec &&
				mock_bluez_media_endpoint1_get_vendor(ep) == vendor) {

			g_autoptr(MockBluezMediaTransport1) transport;
			transport = mock_bluez_device_add_media_transport(self, device_path, transport_path);

			g_autoptr(GVariantBuilder) props = g_variant_builder_new(G_VARIANT_TYPE_VARDICT);
			g_variant_builder_add(props, "{sv}", "Device", g_variant_new_object_path(
						mock_bluez_media_transport1_get_device(transport)));
			g_variant_builder_add(props, "{sv}", "Codec", g_variant_new_byte(codec));
			g_variant_builder_add(props, "{sv}", "Configuration",
						g_variant_new_fixed_byte_array(configuration, configuration_size));
			g_variant_builder_add(props, "{sv}", "State", g_variant_new_string(
						mock_bluez_media_transport1_get_state(transport)));
			g_variant_builder_add(props, "{sv}", "Delay", g_variant_new_uint16(100));
			g_variant_builder_add(props, "{sv}", "Volume", g_variant_new_uint16(50));

			mock_bluez_media_endpoint1_call_set_configuration(ep, transport_path,
					g_variant_builder_end(props), NULL,
					mock_bluez_media_endpoint_set_configuration_finish, ready);

			/* In case of A2DP Sink profile, activate the transport right away. */
			if (strcmp(uuid, BT_UUID_A2DP_SINK) == 0)
				mock_bluez_media_transport1_set_state(transport, "pending");

			/* If enabled, update some properties after given delay. */
			if (self->p.media_transport_update_ms > 0)
				g_timeout_add(self->p.media_transport_update_ms,
						G_SOURCE_FUNC(media_transport_update), transport);

			found = true;
			break;
		}
	}

	g_assert_true(found);

}

void mock_bluez_service_device_add_asha_transport(BlueZMockService * srv,
		const char * device_path, const char * asha_endpoint_path, const char * side,
		bool binaural, const uint8_t sync_id[8]) {
	BlueZMockServicePriv * self = (BlueZMockServicePriv *)srv;

	g_autoptr(MockBluezMediaTransport1) transport;
	g_autofree char * asha_transport_path = g_strconcat(asha_endpoint_path, "/fd0", NULL);

	g_autoptr(MockBluezMediaEndpoint1) endpoint = mock_bluez_media_endpoint1_skeleton_new();
	mock_bluez_media_endpoint1_set_uuid(endpoint, BT_UUID_ASHA);
	mock_bluez_media_endpoint1_set_side(endpoint, side);
	mock_bluez_media_endpoint1_set_binaural(endpoint, binaural);
	mock_bluez_media_endpoint1_set_hi_sync_id(endpoint, g_variant_new_fixed_byte_array(sync_id, 8));
	mock_bluez_media_endpoint1_set_codecs(endpoint, 0x02 /* G722 codec */);
	mock_bluez_media_endpoint1_set_device(endpoint, device_path);
	mock_bluez_media_endpoint1_set_transport(endpoint, asha_transport_path);

	g_autoptr(GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new(asha_endpoint_path);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(endpoint));
	g_dbus_object_manager_server_export(self->server, skeleton);

	transport = mock_bluez_device_add_media_transport(self, device_path, asha_transport_path);
	mock_bluez_media_transport1_set_endpoint(transport, asha_endpoint_path);
	mock_bluez_media_transport1_set_codec(transport, 0x02 /* G722 codec */);

}

static void name_acquired(GDBusConnection * conn,
		G_GNUC_UNUSED const char * name, void * userdata) {
	BlueZMockServicePriv * self = userdata;

	self->server = g_dbus_object_manager_server_new("/");

	mock_bluez_add_profile_manager(self, "/org/bluez");
	mock_bluez_add_adapter(self, MOCK_BLUEZ_ADAPTER_PATH, MOCK_ADAPTER_ADDRESS);

	mock_bluez_adapter_add_device(self, MOCK_BLUEZ_ADAPTER_PATH,
			MOCK_BLUEZ_DEVICE_1_PATH, MOCK_DEVICE_1);
	mock_bluez_adapter_add_device(self, MOCK_BLUEZ_ADAPTER_PATH,
			MOCK_BLUEZ_DEVICE_2_PATH, MOCK_DEVICE_2);

	g_dbus_object_manager_server_set_connection(self->server, conn);
	mock_service_ready(self);
}

static void service_free(void * service) {
	g_autofree BlueZMockServicePriv * self = service;

	g_async_queue_unref(self->p.profile_ready_queue);
	g_async_queue_unref(self->p.media_application_ready_queue);
	g_hash_table_unref(self->profiles);

	g_clear_object(&self->server);
	g_clear_object(&self->media_app_client);
	g_clear_object(&self->gatt_service);
	g_clear_object(&self->gatt_characteristic);
	g_clear_object(&self->advertisement);

}

BlueZMockService * mock_bluez_service_new(void) {

	BlueZMockServicePriv * self = g_new0(BlueZMockServicePriv, 1);
	self->p.service.name = BLUEZ_SERVICE;
	self->p.service.name_acquired_cb = name_acquired;
	self->p.service.free = service_free;

	self->p.profile_ready_queue = g_async_queue_new();
	self->p.media_application_ready_queue = g_async_queue_new();
	self->profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

	return (BlueZMockService *)self;
}
