/*
 * BlueALSA - ofono-iface.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 * Copyright (c) 2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_OFONOIFACE_H_
#define BLUEALSA_OFONOIFACE_H_

#include <gio/gio.h>
#include <glib.h>

#include "dbus.h"

#define OFONO_SERVICE "org.ofono"

#define OFONO_IFACE_MANAGER          OFONO_SERVICE ".Manager"
#define OFONO_IFACE_HF_AUDIO_AGENT   OFONO_SERVICE ".HandsfreeAudioAgent"
#define OFONO_IFACE_HF_AUDIO_CARD    OFONO_SERVICE ".HandsfreeAudioCard"
#define OFONO_IFACE_HF_AUDIO_MANAGER OFONO_SERVICE ".HandsfreeAudioManager"
#define OFONO_IFACE_CALL_VOLUME      OFONO_SERVICE ".CallVolume"

#define OFONO_AUDIO_CARD_TYPE_AG "gateway"
#define OFONO_AUDIO_CARD_TYPE_HF "handsfree"

#define OFONO_AUDIO_CODEC_CVSD    0x01
#define OFONO_AUDIO_CODEC_MSBC    0x02
#define OFONO_AUDIO_CODEC_LC3_SWB 0x03

#define OFONO_MODEM_TYPE_HARDWARE "hardware"
#define OFONO_MODEM_TYPE_HFP      "hfp"
#define OFONO_MODEM_TYPE_SAP      "sap"

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgOfonoHandsfreeAudioAgentSkeleton;

OrgOfonoHandsfreeAudioAgentSkeleton *org_ofono_handsfree_audio_agent_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#endif
