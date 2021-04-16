/*
 * BlueALSA - ba-device.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BADEVICE_H_
#define BLUEALSA_BADEVICE_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <glib.h>

#include "ba-adapter.h"

struct ba_device {

	/* backward reference to adapter */
	struct ba_adapter *a;

	/* address of the Bluetooth device */
	bdaddr_t addr;

	/* connection sequence number */
	unsigned int seq;

	/* data for D-Bus management */
	char *ba_dbus_path;
	char *bluez_dbus_path;

	/* battery level in range [0, 100] or -1 */
	int8_t battery_level;

	/* Apple's extension used with HFP profile */
	struct {

		uint16_t vendor_id;
		uint16_t product_id;
		char software_version[8];
		uint8_t features;

		/* determine whether headset is docked */
		uint8_t accev_docked;

	} xapl;

	/* read-only list of available SEPs */
	const GArray *seps;

	/* hash-map with connected transports */
	pthread_mutex_t transports_mutex;
	GHashTable *transports;

	/* memory self-management */
	int ref_count;

};

struct ba_device *ba_device_new(
		struct ba_adapter *adapter,
		const bdaddr_t *addr);

struct ba_device *ba_device_lookup(
		struct ba_adapter *adapter,
		const bdaddr_t *addr);
struct ba_device *ba_device_ref(
		struct ba_device *d);

void ba_device_destroy(struct ba_device *d);
void ba_device_unref(struct ba_device *d);

#endif
