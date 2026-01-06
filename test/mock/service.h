/*
 * service.h
 * SPDX-FileCopyrightText: 2023-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_MOCK_SERVICE_H_
#define BLUEALSA_MOCK_SERVICE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gio/gio.h>
#include <glib.h>

#define MOCK_ADAPTER_ID 11
#define MOCK_ADAPTER_ADDRESS "00:00:11:11:22:22"
#define MOCK_DEVICE_1 "12:34:56:78:9A:BC"
#define MOCK_DEVICE_2 "23:45:67:89:AB:CD"

#define MOCK_BLUEZ_ADAPTER_PATH "/org/bluez/hci11"
#define MOCK_BLUEZ_DEVICE_1_PATH MOCK_BLUEZ_ADAPTER_PATH "/dev_12_34_56_78_9A_BC"
#define MOCK_BLUEZ_DEVICE_1_SEP_PATH MOCK_BLUEZ_DEVICE_1_PATH "/sep"
#define MOCK_BLUEZ_DEVICE_1_ASHA_PATH MOCK_BLUEZ_DEVICE_1_PATH "/asha"
#define MOCK_BLUEZ_DEVICE_2_PATH MOCK_BLUEZ_ADAPTER_PATH "/dev_23_45_67_89_AB_CD"
#define MOCK_BLUEZ_DEVICE_2_SEP_PATH MOCK_BLUEZ_DEVICE_2_PATH "/sep"
#define MOCK_BLUEZ_MIDI_PATH MOCK_BLUEZ_ADAPTER_PATH "/MIDI"

int channel_drain_callback(GIOChannel * ch, GIOCondition cond, void * userdata);

typedef struct MockService {

	const char * name;
	void (*name_acquired_cb)(GDBusConnection * conn, const char * name, void * userdata);
	void (*name_lost_cb)(GDBusConnection * conn, const char * name, void * userdata);

	GThread * _thread;
	GDBusConnection * _conn;
	GAsyncQueue * _ready;
	GMainLoop * _loop;
	unsigned int _id;

} MockService;

void mock_service_start(MockService * service, GDBusConnection * conn);
void mock_service_ready(MockService * service);
void mock_service_stop(MockService * service);

typedef struct BlueZMockService {
	MockService service;
	/* Queue to notify when new profile has been registered. */
	GAsyncQueue * profile_ready_queue;
	/* Queue to notify when new media application has been registered. */
	GAsyncQueue * media_application_ready_queue;
	/* If non-zero, update media transport properties after given time. */
	unsigned int media_transport_update_ms;
	/* Service private data. */
	void * priv;
} BlueZMockService;

int mock_bluez_service_add_device_name_mapping(const char * mapping);
BlueZMockService * mock_bluez_service_new(void);
void mock_bluez_service_device_add_media_endpoint(BlueZMockService * srv,
		const char * device_path, const char * endpoint_path, const char * uuid,
		uint32_t codec_id, const void * capabilities, size_t capabilities_size);
void mock_bluez_service_device_add_asha_transport(BlueZMockService * srv,
		const char * device_path, const char * asha_endpoint_path, const char * side,
		bool binaural, const uint8_t sync_id[8]);
void mock_bluez_service_device_profile_new_connection(BlueZMockService * srv,
		const char * device_path, const char * uuid, GAsyncQueue * ready);
void mock_bluez_service_device_media_set_configuration(BlueZMockService * srv,
		const char * device_path, const char * transport_path, const char *uuid,
		uint32_t codec_id, const void * configuration, size_t configuration_size,
		GAsyncQueue * ready);
void mock_bluez_service_free(BlueZMockService * srv);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BlueZMockService, mock_bluez_service_free)

typedef struct OFonoMockService {
	MockService service;
	/* Service private data. */
	void * priv;
} OFonoMockService;

OFonoMockService * mock_ofono_service_new(void);
void mock_ofono_service_free(OFonoMockService * srv);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OFonoMockService, mock_ofono_service_free)

typedef struct UPowerMockService {
	MockService service;
	/* Service private data. */
	void * priv;
} UPowerMockService;

UPowerMockService * mock_upower_service_new(void);
void mock_upower_service_display_device_set_is_present(UPowerMockService * srv,
		bool present);
void mock_upower_service_display_device_set_percentage(UPowerMockService * srv,
		double percentage);
void mock_upower_service_free(UPowerMockService * srv);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(UPowerMockService, mock_upower_service_free)

#endif
