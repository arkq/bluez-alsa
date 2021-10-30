/*
 * BlueALSA - ofono-skeleton.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_OFONOSKELETON_H_
#define BLUEALSA_OFONOSKELETON_H_

#include <gio/gio.h>
#include <glib.h>

#include "dbus.h"

typedef struct {
	GDBusInterfaceSkeletonClass parent;
} ofono_HFAudioAgentIfaceSkeletonClass;

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} ofono_HFAudioAgentIfaceSkeleton;

ofono_HFAudioAgentIfaceSkeleton *ofono_hf_audio_agent_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#endif
