/*
 * BlueALSA - ba-device.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
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
	char *ba_battery_dbus_path;
	char *bluez_dbus_path;
	/* string representation of BT address */
	char addr_dbus_str[sizeof("dev_XX_XX_XX_XX_XX_XX")];

	struct {
		/* battery parameters in range [0, 100] or -1 */
		int8_t charge;
		int8_t health;
	} battery;

	/* Apple's extension used with HFP profile */
	struct {

		uint16_t vendor_id;
		uint16_t product_id;
		uint16_t sw_version;
		uint8_t features;

		/* determine whether headset is docked */
		uint8_t accev_docked;

	} xapl;

	/* read-only list of available SEP configurations */
	const GArray *sep_configs;

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
		const struct ba_adapter *adapter,
		const bdaddr_t *addr);
struct ba_device *ba_device_ref(
		struct ba_device *d);

void ba_device_destroy(struct ba_device *d);
void ba_device_unref(struct ba_device *d);

/**
 * Return the device SEP configuration as the given index. */
#define ba_device_sep_cfg_array_index(sep_configs, i) \
	g_array_index(sep_configs, struct a2dp_sep_config, i)

#endif
