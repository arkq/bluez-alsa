/*
 * BlueALSA - bluez-iface.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BLUEZIFACE_H_
#define BLUEALSA_BLUEZIFACE_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <gio/gio.h>
#include <glib.h>

#include "dbus.h"

#define BLUEZ_SERVICE "org.bluez"

#define BLUEZ_IFACE_ADAPTER                  BLUEZ_SERVICE ".Adapter1"
#define BLUEZ_IFACE_BATTERY_PROVIDER         BLUEZ_SERVICE ".BatteryProvider1"
#define BLUEZ_IFACE_BATTERY_PROVIDER_MANAGER BLUEZ_SERVICE ".BatteryProviderManager1"
#define BLUEZ_IFACE_DEVICE                   BLUEZ_SERVICE ".Device1"
#define BLUEZ_IFACE_GATT_CHARACTERISTIC      BLUEZ_SERVICE ".GattCharacteristic1"
#define BLUEZ_IFACE_GATT_MANAGER             BLUEZ_SERVICE ".GattManager1"
#define BLUEZ_IFACE_GATT_PROFILE             BLUEZ_SERVICE ".GattProfile1"
#define BLUEZ_IFACE_GATT_SERVICE             BLUEZ_SERVICE ".GattService1"
#define BLUEZ_IFACE_LE_ADVERTISEMENT         BLUEZ_SERVICE ".LEAdvertisement1"
#define BLUEZ_IFACE_LE_ADVERTISING_MANAGER   BLUEZ_SERVICE ".LEAdvertisingManager1"
#define BLUEZ_IFACE_MEDIA                    BLUEZ_SERVICE ".Media1"
#define BLUEZ_IFACE_MEDIA_ENDPOINT           BLUEZ_SERVICE ".MediaEndpoint1"
#define BLUEZ_IFACE_MEDIA_TRANSPORT          BLUEZ_SERVICE ".MediaTransport1"
#define BLUEZ_IFACE_PROFILE                  BLUEZ_SERVICE ".Profile1"
#define BLUEZ_IFACE_PROFILE_MANAGER          BLUEZ_SERVICE ".ProfileManager1"

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluezBatteryProvider1Skeleton;

OrgBluezBatteryProvider1Skeleton *org_bluez_battery_provider1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#if ENABLE_MIDI

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluezGattCharacteristic1Skeleton;

OrgBluezGattCharacteristic1Skeleton *org_bluez_gatt_characteristic1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluezGattService1Skeleton;

OrgBluezGattService1Skeleton *org_bluez_gatt_service1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluezLeadvertisement1Skeleton;

OrgBluezLeadvertisement1Skeleton *org_bluez_leadvertisement1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#endif

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluezMediaEndpoint1Skeleton;

OrgBluezMediaEndpoint1Skeleton *org_bluez_media_endpoint1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluezProfile1Skeleton;

OrgBluezProfile1Skeleton *org_bluez_profile1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#endif
