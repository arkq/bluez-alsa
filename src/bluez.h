/*
 * BlueALSA - bluez.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEZ_H_
#define BLUEALSA_BLUEZ_H_

#include <stdbool.h>

#include <glib.h>

#include "a2dp.h"

/* List of Bluetooth audio profiles. */
#define BLUETOOTH_UUID_A2DP_SOURCE "0000110A-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_A2DP_SINK   "0000110B-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HSP_HS      "00001108-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HSP_AG      "00001112-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HFP_HF      "0000111E-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HFP_AG      "0000111F-0000-1000-8000-00805F9B34FB"

enum bluez_a2dp_transport_state {
	BLUEZ_A2DP_TRANSPORT_STATE_IDLE,
	BLUEZ_A2DP_TRANSPORT_STATE_PENDING,
	BLUEZ_A2DP_TRANSPORT_STATE_ACTIVE,
};

void bluez_register(void);
int bluez_subscribe_signals(void);

bool bluez_a2dp_set_configuration(
		const char *dbus_current_sep_path,
		const struct a2dp_sep *sep,
		GError **error);

#endif
