/*
 * BlueALSA - bluealsa-iface.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-iface.h"

static const GDBusArgInfo arg_path = {
	-1, "path", "o", NULL
};

static const GDBusArgInfo arg_PCMs = {
	-1, "PCMs", "a{oa{sv}}", NULL
};

static const GDBusArgInfo arg_props = {
	-1, "props", "a{sv}", NULL
};

static const GDBusArgInfo *out_GetPCMs[] = {
	&arg_PCMs,
	NULL,
};

static const GDBusMethodInfo bluealsa_iface_manager_GetPCMs = {
	-1, "GetPCMs",
	NULL,
	(GDBusArgInfo **)out_GetPCMs,
	NULL,
};

static const GDBusMethodInfo *bluealsa_iface_manager_methods[] = {
	&bluealsa_iface_manager_GetPCMs,
	NULL,
};

static const GDBusArgInfo *args_PCMAdded[] = {
	&arg_path,
	&arg_props,
	NULL,
};

static const GDBusArgInfo *args_PCMRemoved[] = {
	&arg_path,
	NULL,
};

static const GDBusSignalInfo bluealsa_iface_manager_PCMAdded = {
	-1, "PCMAdded",
	(GDBusArgInfo **)args_PCMAdded,
	NULL,
};

static const GDBusSignalInfo bluealsa_iface_manager_PCMRemoved = {
	-1, "PCMRemoved",
	(GDBusArgInfo **)args_PCMRemoved,
	NULL,
};

static const GDBusSignalInfo *bluealsa_iface_manager_signals[] = {
	&bluealsa_iface_manager_PCMAdded,
	&bluealsa_iface_manager_PCMRemoved,
	NULL,
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Device = {
	-1, "Device", "o", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Modes = {
	-1, "Modes", "as", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Channels = {
	-1, "Channels", "y", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Sampling = {
	-1, "Sampling", "u", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo *bluealsa_iface_pcm_properties[] = {
	&bluealsa_iface_pcm_Device,
	&bluealsa_iface_pcm_Modes,
	&bluealsa_iface_pcm_Channels,
	&bluealsa_iface_pcm_Sampling,
	NULL,
};

const GDBusInterfaceInfo bluealsa_iface_manager = {
	-1, BLUEALSA_IFACE_MANAGER,
	(GDBusMethodInfo **)bluealsa_iface_manager_methods,
	(GDBusSignalInfo **)bluealsa_iface_manager_signals,
	NULL,
	NULL,
};

const GDBusInterfaceInfo bluealsa_iface_pcm = {
	-1, BLUEALSA_IFACE_PCM,
	NULL,
	NULL,
	(GDBusPropertyInfo **)bluealsa_iface_pcm_properties,
	NULL,
};
