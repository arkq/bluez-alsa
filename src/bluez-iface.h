/*
 * BlueALSA - bluez-iface.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BLUEZIFACE_H_
#define BLUEALSA_BLUEZIFACE_H_

#include "dbus/org.bluez.h"

#define BLUEZ_SERVICE "org.bluez"

#define BLUEZ_IFACE_ADAPTER                     BLUEZ_SERVICE ".Adapter1"
#define BLUEZ_IFACE_BATTERY_PROVIDER            BLUEZ_SERVICE ".BatteryProvider1"
#define BLUEZ_IFACE_BATTERY_PROVIDER_MANAGER    BLUEZ_SERVICE ".BatteryProviderManager1"
#define BLUEZ_IFACE_DEVICE                      BLUEZ_SERVICE ".Device1"
#define BLUEZ_IFACE_GATT_CHARACTERISTIC         BLUEZ_SERVICE ".GattCharacteristic1"
#define BLUEZ_IFACE_GATT_MANAGER                BLUEZ_SERVICE ".GattManager1"
#define BLUEZ_IFACE_GATT_PROFILE                BLUEZ_SERVICE ".GattProfile1"
#define BLUEZ_IFACE_GATT_SERVICE                BLUEZ_SERVICE ".GattService1"
#define BLUEZ_IFACE_LE_ADVERTISEMENT            BLUEZ_SERVICE ".LEAdvertisement1"
#define BLUEZ_IFACE_LE_ADVERTISING_MANAGER      BLUEZ_SERVICE ".LEAdvertisingManager1"
#define BLUEZ_IFACE_MEDIA                       BLUEZ_SERVICE ".Media1"
#define BLUEZ_IFACE_MEDIA_ENDPOINT              BLUEZ_SERVICE ".MediaEndpoint1"
#define BLUEZ_IFACE_MEDIA_TRANSPORT             BLUEZ_SERVICE ".MediaTransport1"
#define BLUEZ_IFACE_PROFILE                     BLUEZ_SERVICE ".Profile1"
#define BLUEZ_IFACE_PROFILE_MANAGER             BLUEZ_SERVICE ".ProfileManager1"

#define BLUEZ_ERROR                             "org.bluez.Error"
#define BLUEZ_ERROR_FAILED                      BLUEZ_ERROR ".Failed"
#define BLUEZ_ERROR_INVALID_ARGUMENTS           BLUEZ_ERROR ".InvalidArguments"
#define BLUEZ_ERROR_A2DP_INVALID_CODEC_TYPE     BLUEZ_ERROR ".A2DP.InvalidCodecType"
#define BLUEZ_ERROR_A2DP_INVALID_CODEC_PARAM    BLUEZ_ERROR ".A2DP.InvalidCodecParameter"
#define BLUEZ_ERROR_A2DP_INVALID_CHANNELS       BLUEZ_ERROR ".A2DP.InvalidChannels"
#define BLUEZ_ERROR_A2DP_INVALID_CHANNEL_MODE   BLUEZ_ERROR ".A2DP.InvalidChannelMode"
#define BLUEZ_ERROR_A2DP_INVALID_SAMPLING_FREQ  BLUEZ_ERROR ".A2DP.InvalidSamplingFrequency"
#define BLUEZ_ERROR_A2DP_INVALID_BLOCK_LENGTH   BLUEZ_ERROR ".A2DP.InvalidBlockLength"
#define BLUEZ_ERROR_A2DP_INVALID_SUB_BANDS      BLUEZ_ERROR ".A2DP.InvalidSubbands"
#define BLUEZ_ERROR_A2DP_INVALID_ALLOC_METHOD   BLUEZ_ERROR ".A2DP.InvalidAllocationMethod"
#define BLUEZ_ERROR_A2DP_INVALID_MIN_BIT_POOL   BLUEZ_ERROR ".A2DP.InvalidMinimumBitpoolValue"
#define BLUEZ_ERROR_A2DP_INVALID_MAX_BIT_POOL   BLUEZ_ERROR ".A2DP.InvalidMaximumBitpoolValue"
#define BLUEZ_ERROR_A2DP_INVALID_LAYER          BLUEZ_ERROR ".A2DP.InvalidLayer"
#define BLUEZ_ERROR_A2DP_INVALID_OBJECT_TYPE    BLUEZ_ERROR ".A2DP.InvalidObjectType"

#endif
