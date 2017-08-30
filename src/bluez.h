/*
 * BlueALSA - bluez.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEZ_H_
#define BLUEALSA_BLUEZ_H_

/* List of Bluetoth audio profiles. */
#define BLUETOOTH_UUID_A2DP_SOURCE "0000110A-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_A2DP_SINK   "0000110B-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HSP_HS      "00001108-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HSP_AG      "00001112-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HFP_HF      "0000111E-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HFP_AG      "0000111F-0000-1000-8000-00805F9B34FB"

enum bluetooth_profile {
	BLUETOOTH_PROFILE_NULL = 0,
	BLUETOOTH_PROFILE_A2DP_SOURCE,
	BLUETOOTH_PROFILE_A2DP_SINK,
	BLUETOOTH_PROFILE_HSP_HS,
	BLUETOOTH_PROFILE_HSP_AG,
	BLUETOOTH_PROFILE_HFP_HF,
	BLUETOOTH_PROFILE_HFP_AG,
};

void bluez_register_a2dp(void);
void bluez_register_hfp(void);
int bluez_subscribe_signals(void);

#endif
