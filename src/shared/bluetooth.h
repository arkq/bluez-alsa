/*
 * BlueALSA - bluetooth.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_BLUETOOTH_H_
#define BLUEALSA_SHARED_BLUETOOTH_H_

/**
 * List of all Bluetooth member companies:
 * https://www.bluetooth.com/specifications/assigned-numbers/company-identifiers */

#define BT_COMPID_INTEL              0x0002
#define BT_COMPID_QUALCOMM_TECH_INTL 0x000A
#define BT_COMPID_BROADCOM           0x000F
#define BT_COMPID_APPLE              0x004C
#define BT_COMPID_APT                0x004F
#define BT_COMPID_REALTEK            0x005D
#define BT_COMPID_SAMSUNG_ELEC       0x0075
#define BT_COMPID_QUALCOMM_TECH      0x00D7
#define BT_COMPID_GOOGLE             0x00E0
#define BT_COMPID_SONY               0x012D
#define BT_COMPID_CYPRESS            0x0131
#define BT_COMPID_SAVITECH           0x053A
#define BT_COMPID_LINUX_FOUNDATION   0x05F1
#define BT_COMPID_FRAUNHOFER_IIS     0x08A9

/**
 * Bluetooth UUIDs associated with audio profiles. */

#define BT_UUID_A2DP_SOURCE "0000110a-0000-1000-8000-00805f9b34fb"
#define BT_UUID_A2DP_SINK   "0000110b-0000-1000-8000-00805f9b34fb"
#define BT_UUID_HSP_HS      "00001108-0000-1000-8000-00805f9b34fb"
#define BT_UUID_HSP_AG      "00001112-0000-1000-8000-00805f9b34fb"
#define BT_UUID_HFP_HF      "0000111e-0000-1000-8000-00805f9b34fb"
#define BT_UUID_HFP_AG      "0000111f-0000-1000-8000-00805f9b34fb"

#define BT_UUID_MIDI        "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define BT_UUID_MIDI_CHAR   "7772e5db-3868-4112-a1a9-f2669d106bf3"
#define BT_UUID_MIDI_DESC   "00002901-0000-1000-8000-00805f9b34fb"

#endif
