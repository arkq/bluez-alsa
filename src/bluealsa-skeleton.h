/*
 * BlueALSA - bluealsa-skeleton.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BLUEALSASKELETON_H_
#define BLUEALSA_BLUEALSASKELETON_H_

#include <gio/gio.h>
#include <glib.h>

#include "dbus.h"

typedef struct {
	GDBusInterfaceSkeletonClass parent;
} bluealsa_ManagerIfaceSkeletonClass;

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} bluealsa_ManagerIfaceSkeleton;

bluealsa_ManagerIfaceSkeleton *bluealsa_manager_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonClass parent;
} bluealsa_PCMIfaceSkeletonClass;

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} bluealsa_PCMIfaceSkeleton;

bluealsa_PCMIfaceSkeleton *bluealsa_pcm_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonClass parent;
} bluealsa_RFCOMMIfaceSkeletonClass;

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} bluealsa_RFCOMMIfaceSkeleton;

bluealsa_RFCOMMIfaceSkeleton *bluealsa_rfcomm_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#endif
