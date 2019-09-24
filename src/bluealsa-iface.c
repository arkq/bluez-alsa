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

static const GDBusArgInfo arg_fd = {
	-1, "fd", "h", NULL
};

static const GDBusArgInfo arg_mode = {
	-1, "mode", "s", NULL
};

static const GDBusArgInfo arg_path = {
	-1, "path", "o", NULL
};

static const GDBusArgInfo arg_PCMs = {
	-1, "PCMs", "a{oa{sv}}", NULL
};

static const GDBusArgInfo arg_props = {
	-1, "props", "a{sv}", NULL
};

static const GDBusArgInfo *GetPCMs_out[] = {
	&arg_PCMs,
	NULL,
};

static const GDBusMethodInfo bluealsa_iface_manager_GetPCMs = {
	-1, "GetPCMs",
	NULL,
	(GDBusArgInfo **)GetPCMs_out,
	NULL,
};

static const GDBusMethodInfo *bluealsa_iface_manager_methods[] = {
	&bluealsa_iface_manager_GetPCMs,
	NULL,
};

static const GDBusArgInfo *PCMAdded_args[] = {
	&arg_path,
	&arg_props,
	NULL,
};

static const GDBusArgInfo *PCMRemoved_args[] = {
	&arg_path,
	NULL,
};

static const GDBusSignalInfo bluealsa_iface_manager_PCMAdded = {
	-1, "PCMAdded",
	(GDBusArgInfo **)PCMAdded_args,
	NULL,
};

static const GDBusSignalInfo bluealsa_iface_manager_PCMRemoved = {
	-1, "PCMRemoved",
	(GDBusArgInfo **)PCMRemoved_args,
	NULL,
};

static const GDBusSignalInfo *bluealsa_iface_manager_signals[] = {
	&bluealsa_iface_manager_PCMAdded,
	&bluealsa_iface_manager_PCMRemoved,
	NULL,
};

static const GDBusArgInfo *pcm_Open_in[] = {
	&arg_mode,
	NULL,
};

static const GDBusArgInfo *pcm_Open_out[] = {
	&arg_fd,
	&arg_fd,
	NULL,
};

static const GDBusMethodInfo bluealsa_iface_pcm_Open = {
	-1, "Open",
	(GDBusArgInfo **)pcm_Open_in,
	(GDBusArgInfo **)pcm_Open_out,
	NULL,
};

static const GDBusMethodInfo *bluealsa_iface_pcm_methods[] = {
	&bluealsa_iface_pcm_Open,
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

static const GDBusPropertyInfo bluealsa_iface_pcm_Format = {
	-1, "Format", "q", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Codec = {
	-1, "Codec", "q", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Delay = {
	-1, "Delay", "q", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Volume = {
	-1, "Volume", "q",
	G_DBUS_PROPERTY_INFO_FLAGS_READABLE |
	G_DBUS_PROPERTY_INFO_FLAGS_WRITABLE,
	NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Battery = {
	-1, "Battery", "y", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo *bluealsa_iface_pcm_properties[] = {
	&bluealsa_iface_pcm_Device,
	&bluealsa_iface_pcm_Modes,
	&bluealsa_iface_pcm_Channels,
	&bluealsa_iface_pcm_Sampling,
	&bluealsa_iface_pcm_Codec,
	&bluealsa_iface_pcm_Delay,
	&bluealsa_iface_pcm_Volume,
	&bluealsa_iface_pcm_Battery,
	NULL,
};

static const GDBusArgInfo *rfcomm_Open_out[] = {
	&arg_fd,
	NULL,
};

static const GDBusMethodInfo bluealsa_iface_rfcomm_Open = {
	-1, "Open",
	NULL,
	(GDBusArgInfo **)rfcomm_Open_out,
	NULL,
};

static const GDBusMethodInfo *bluealsa_iface_rfcomm_methods[] = {
	&bluealsa_iface_rfcomm_Open,
	NULL,
};

static const GDBusPropertyInfo bluealsa_iface_rfcomm_Mode = {
	-1, "Mode", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_rfcomm_Features = {
	-1, "Features", "u", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo *bluealsa_iface_rfcomm_properties[] = {
	&bluealsa_iface_rfcomm_Mode,
	&bluealsa_iface_rfcomm_Features,
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
	(GDBusMethodInfo **)bluealsa_iface_pcm_methods,
	NULL,
	(GDBusPropertyInfo **)bluealsa_iface_pcm_properties,
	NULL,
};

const GDBusInterfaceInfo bluealsa_iface_rfcomm = {
	-1, BLUEALSA_IFACE_RFCOMM,
	(GDBusMethodInfo **)bluealsa_iface_rfcomm_methods,
	NULL,
	(GDBusPropertyInfo **)bluealsa_iface_rfcomm_properties,
	NULL,
};
