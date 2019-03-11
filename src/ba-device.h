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

#include <stdbool.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <glib.h>

#include "ba-adapter.h"

struct ba_device {

	/* backward reference to adapter */
	struct ba_adapter *a;

	/* address of the Bluetooth device */
	bdaddr_t addr;
	/* human-readable Bluetooth device name */
	char name[HCI_MAX_NAME_LENGTH];

	/* adjusted (in the range 0-100) battery level */
	struct {
		bool enabled;
		uint8_t level;
	} battery;

	/* Apple's extension used with HFP profile */
	struct {

		uint16_t vendor_id;
		uint16_t product_id;
		uint16_t version;
		uint8_t features;

		/* determine whether headset is docked */
		uint8_t accev_docked;

	} xapl;

	/* hash-map with connected transports */
	GHashTable *transports;

};

struct ba_device *ba_device_new(
		struct ba_adapter *adapter,
		const bdaddr_t *addr,
		const char *name);

struct ba_device *ba_device_lookup(
		struct ba_adapter *adapter,
		const bdaddr_t *addr);

void ba_device_free(struct ba_device *d);

void ba_device_set_battery_level(struct ba_device *d, uint8_t value);
void ba_device_set_name(struct ba_device *d, const char *name);

#endif
