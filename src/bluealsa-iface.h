/*
 * BlueALSA - bluealsa-iface.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BLUEALSAIFACE_H_
#define BLUEALSA_BLUEALSAIFACE_H_

#include <gio/gio.h>
#include <glib.h>

#include "dbus.h"

#define BLUEALSA_SERVICE "org.bluealsa"

#define BLUEALSA_IFACE_MANAGER BLUEALSA_SERVICE ".Manager1"
#define BLUEALSA_IFACE_PCM     BLUEALSA_SERVICE ".PCM1"
#define BLUEALSA_IFACE_RFCOMM  BLUEALSA_SERVICE ".RFCOMM1"

#define BLUEALSA_TRANSPORT_TYPE_A2DP        "A2DP"
#define BLUEALSA_TRANSPORT_TYPE_A2DP_SOURCE BLUEALSA_TRANSPORT_TYPE_A2DP "-source"
#define BLUEALSA_TRANSPORT_TYPE_A2DP_SINK   BLUEALSA_TRANSPORT_TYPE_A2DP "-sink"
#define BLUEALSA_TRANSPORT_TYPE_HFP         "HFP"
#define BLUEALSA_TRANSPORT_TYPE_HFP_OFONO   BLUEALSA_TRANSPORT_TYPE_HFP "-oFono"
#define BLUEALSA_TRANSPORT_TYPE_HFP_AG      BLUEALSA_TRANSPORT_TYPE_HFP "-AG"
#define BLUEALSA_TRANSPORT_TYPE_HFP_HF      BLUEALSA_TRANSPORT_TYPE_HFP "-HF"
#define BLUEALSA_TRANSPORT_TYPE_HSP         "HSP"
#define BLUEALSA_TRANSPORT_TYPE_HSP_AG      BLUEALSA_TRANSPORT_TYPE_HSP "-AG"
#define BLUEALSA_TRANSPORT_TYPE_HSP_HS      BLUEALSA_TRANSPORT_TYPE_HSP "-HS"
#define BLUEALSA_TRANSPORT_TYPE_MIDI        "MIDI"

#define BLUEALSA_PCM_CTRL_DRAIN  "Drain"
#define BLUEALSA_PCM_CTRL_DROP   "Drop"
#define BLUEALSA_PCM_CTRL_PAUSE  "Pause"
#define BLUEALSA_PCM_CTRL_RESUME "Resume"

#define BLUEALSA_PCM_MODE_SINK   "sink"
#define BLUEALSA_PCM_MODE_SOURCE "source"

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluealsaManager1Skeleton;

OrgBluealsaManager1Skeleton *org_bluealsa_manager1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluealsaPcm1Skeleton;

OrgBluealsaPcm1Skeleton *org_bluealsa_pcm1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgBluealsaRfcomm1Skeleton;

OrgBluealsaRfcomm1Skeleton *org_bluealsa_rfcomm1_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#endif
