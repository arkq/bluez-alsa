/*
 * BlueALSA - bluetooth.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
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

#define BT_UUID_ASHA        "0000fdf0-0000-1000-8000-00805f9b34fb"
#define BT_UUID_ASHA_PROPS  "6333651e-c481-4a3e-9169-7c902aad37bb"
#define BT_UUID_ASHA_CTRL   "f0d4de7e-4a88-476c-9d9f-1937b0996cc0"
#define BT_UUID_ASHA_STATUS "38663f1a-e711-4cac-b641-326b56404837"
#define BT_UUID_ASHA_VOLUME "00e4ca9e-ab14-41e4-8823-f9e70c7e91df"
#define BT_UUID_ASHA_PSM    "2d410339-82b6-42aa-b34e-e2e01df8cc1a"

#define BT_UUID_MIDI        "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define BT_UUID_MIDI_CHAR   "7772e5db-3868-4112-a1a9-f2669d106bf3"
#define BT_UUID_MIDI_DESC   "00002901-0000-1000-8000-00805f9b34fb"

#endif
