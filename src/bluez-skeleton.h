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

#include "ba-device.h"

typedef struct {
	GDBusInterfaceSkeletonClass parent;
} bluez_BatteryProviderIfaceSkeletonClass;

typedef struct {
	GDBusInterfaceSkeleton parent;
	struct ba_device *device;
} bluez_BatteryProviderIfaceSkeleton;

bluez_BatteryProviderIfaceSkeleton *bluez_battery_provider_iface_skeleton_new(
		struct ba_device *device);

#endif
