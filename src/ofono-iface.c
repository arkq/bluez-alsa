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

static const GDBusMethodInfo ofono_iface_profile_NewConnection = {
	-1, "NewConnection",
	(GDBusArgInfo **)in_NewConnection,
	NULL,
	NULL,
};

static const GDBusMethodInfo ofono_iface_profile_Release = {
	-1, "Release",
	NULL,
	NULL,
	NULL,
};

static const GDBusMethodInfo *ofono_iface_profile_methods[] = {
	&ofono_iface_profile_NewConnection,
	&ofono_iface_profile_Release,
	NULL,
};


GDBusInterfaceInfo ofono_iface_profile = {
	-1, HF_AUDIO_AGENT_INTERFACE,
	(GDBusMethodInfo **)ofono_iface_profile_methods,
	NULL,
	NULL,
	NULL,
};
