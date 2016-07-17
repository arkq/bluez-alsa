/*
 * bluealsa - bluez.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEZ_H_
#define BLUEALSA_BLUEZ_H_

#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>
#include <glib.h>

#define BLUETOOTH_UUID_A2DP_SOURCE "0000110a-0000-1000-8000-00805f9b34fb"
#define BLUETOOTH_UUID_A2DP_SINK   "0000110b-0000-1000-8000-00805f9b34fb"
#define BLUETOOTH_UUID_HSP_HS      "00001108-0000-1000-8000-00805f9b34fb"
#define BLUETOOTH_UUID_HSP_AG      "00001112-0000-1000-8000-00805f9b34fb"
#define BLUETOOTH_UUID_HFP_HF      "0000111e-0000-1000-8000-00805f9b34fb"
#define BLUETOOTH_UUID_HFP_AG      "0000111f-0000-1000-8000-00805f9b34fb"

#define ENDPOINT_A2DP_SOURCE "/MediaEndpoint/A2DPSource"
#define ENDPOINT_A2DP_SINK   "/MediaEndpoint/A2DPSink"
#define PROFILE_HSP_AG       "/Profile/HSPAGProfile"

struct ba_device {

	bdaddr_t addr;
	char *name;

	/* collection of connected transports */
	GHashTable *transports;

};

struct ba_device *bluez_device_new(bdaddr_t *addr, const char *name);
void bluez_device_free(struct ba_device *d);

int bluez_register_endpoint(DBusConnection *conn, const char *device, const char *endpoint, const char *uuid);
int bluez_register_profile(DBusConnection *conn, const char *profile, const char *uuid);

int bluez_register_a2dp_source(DBusConnection *conn, const char *device, void *userdata);
int bluez_register_a2dp_sink(DBusConnection *conn, const char *device, void *userdata);
int bluez_register_profile_hsp_ag(DBusConnection *conn, void *userdata);

int bluez_register_signal_handler(DBusConnection *conn, const char *device, void *userdata);

/* Helper macro for proper initialization of the device list structure. */
#define bluez_devices_init() \
	g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)bluez_device_free)

#endif
