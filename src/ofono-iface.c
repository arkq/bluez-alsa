/*
 * BlueALSA - ofono-iface.c
 * Copyright (c) 2018 Thierry Bultel (thierry.bultel@iot.bzh)
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ofono-iface.h"

#include <stddef.h>

static const GDBusArgInfo arg_card = {
	-1, "card", "o", NULL
};

static const GDBusArgInfo arg_fd = {
	-1, "fd", "h", NULL
};

static const GDBusArgInfo arg_codec = {
	-1, "codec", "y", NULL
};

static const GDBusArgInfo *in_NewConnection[] = {
	&arg_card,
	&arg_fd,
	&arg_codec,
	NULL,
};

static const GDBusMethodInfo ofono_iface_hf_audio_agent_NewConnection = {
	-1, "NewConnection",
	(GDBusArgInfo **)in_NewConnection,
	NULL,
	NULL,
};

static const GDBusMethodInfo ofono_iface_hf_audio_agent_Release = {
	-1, "Release",
	NULL,
	NULL,
	NULL,
};

static const GDBusMethodInfo *ofono_iface_hf_audio_agent_methods[] = {
	&ofono_iface_hf_audio_agent_NewConnection,
	&ofono_iface_hf_audio_agent_Release,
	NULL,
};


const GDBusInterfaceInfo ofono_iface_hf_audio_agent = {
	-1, OFONO_IFACE_HF_AUDIO_AGENT,
	(GDBusMethodInfo **)ofono_iface_hf_audio_agent_methods,
	NULL,
	NULL,
	NULL,
};
