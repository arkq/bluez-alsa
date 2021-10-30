/*
 * BlueALSA - bluez-skeleton.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BLUEZSKELETON_H_
#define BLUEALSA_BLUEZSKELETON_H_

#include <gio/gio.h>
#include <glib.h>

#include "dbus.h"

typedef struct {
	GDBusInterfaceSkeletonClass parent;
} bluez_BatteryProviderIfaceSkeletonClass;

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} bluez_BatteryProviderIfaceSkeleton;

bluez_BatteryProviderIfaceSkeleton *bluez_battery_provider_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonClass parent;
} bluez_MediaEndpointIfaceSkeletonClass;

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} bluez_MediaEndpointIfaceSkeleton;

bluez_MediaEndpointIfaceSkeleton *bluez_media_endpoint_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonClass parent;
} bluez_ProfileIfaceSkeletonClass;

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} bluez_ProfileIfaceSkeleton;

bluez_ProfileIfaceSkeleton *bluez_profile_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#endif
