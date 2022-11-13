/*
 * BlueALSA - bluealsa-iface.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-iface.h"

#include <stddef.h>

static const GDBusArgInfo arg_codec = {
	-1, "codec", "s", NULL
};

static const GDBusArgInfo arg_codecs = {
	-1, "codecs", "a{sa{sv}}", NULL
};

static const GDBusArgInfo arg_fd = {
	-1, "fd", "h", NULL
};

static const GDBusArgInfo arg_props = {
	-1, "props", "a{sv}", NULL
};

static const GDBusPropertyInfo bluealsa_iface_manager_Version = {
	-1, "Version", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_manager_Adapters = {
	-1, "Adapters", "as", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_manager_Profiles = {
	-1, "Profiles", "as", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_manager_Codecs = {
	-1, "Codecs", "as", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo *bluealsa_iface_manager_properties[] = {
	&bluealsa_iface_manager_Version,
	&bluealsa_iface_manager_Adapters,
	&bluealsa_iface_manager_Profiles,
	&bluealsa_iface_manager_Codecs,
	NULL,
};

static const GDBusArgInfo *pcm_Open_out[] = {
	&arg_fd,
	&arg_fd,
	NULL,
};

static const GDBusArgInfo *pcm_GetCodecs_out[] = {
	&arg_codecs,
	NULL,
};

static const GDBusArgInfo *pcm_SelectCodec_in[] = {
	&arg_codec,
	&arg_props,
	NULL,
};

static const GDBusMethodInfo bluealsa_iface_pcm_Open = {
	-1, "Open",
	NULL,
	(GDBusArgInfo **)pcm_Open_out,
	NULL,
};

static const GDBusMethodInfo bluealsa_iface_pcm_GetCodecs = {
	-1, "GetCodecs",
	NULL,
	(GDBusArgInfo **)pcm_GetCodecs_out,
	NULL,
};

static const GDBusMethodInfo bluealsa_iface_pcm_SelectCodec = {
	-1, "SelectCodec",
	(GDBusArgInfo **)pcm_SelectCodec_in,
	NULL,
	NULL,
};

static const GDBusMethodInfo *bluealsa_iface_pcm_methods[] = {
	&bluealsa_iface_pcm_Open,
	&bluealsa_iface_pcm_GetCodecs,
	&bluealsa_iface_pcm_SelectCodec,
	NULL,
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Device = {
	-1, "Device", "o", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Sequence = {
	-1, "Sequence", "u", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Transport = {
	-1, "Transport", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Mode = {
	-1, "Mode", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Running = {
	-1, "Running", "b", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Format = {
	-1, "Format", "q", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Channels = {
	-1, "Channels", "y", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Sampling = {
	-1, "Sampling", "u", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Codec = {
	-1, "Codec", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_CodecConfiguration = {
	-1, "CodecConfiguration", "ay", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Delay = {
	-1, "Delay", "q", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_SoftVolume = {
	-1, "SoftVolume", "b",
	G_DBUS_PROPERTY_INFO_FLAGS_READABLE |
	G_DBUS_PROPERTY_INFO_FLAGS_WRITABLE,
	NULL
};

static const GDBusPropertyInfo bluealsa_iface_pcm_Volume = {
	-1, "Volume", "q",
	G_DBUS_PROPERTY_INFO_FLAGS_READABLE |
	G_DBUS_PROPERTY_INFO_FLAGS_WRITABLE,
	NULL
};

static const GDBusPropertyInfo *bluealsa_iface_pcm_properties[] = {
	&bluealsa_iface_pcm_Device,
	&bluealsa_iface_pcm_Sequence,
	&bluealsa_iface_pcm_Transport,
	&bluealsa_iface_pcm_Mode,
	&bluealsa_iface_pcm_Running,
	&bluealsa_iface_pcm_Format,
	&bluealsa_iface_pcm_Channels,
	&bluealsa_iface_pcm_Sampling,
	&bluealsa_iface_pcm_Codec,
	&bluealsa_iface_pcm_CodecConfiguration,
	&bluealsa_iface_pcm_Delay,
	&bluealsa_iface_pcm_SoftVolume,
	&bluealsa_iface_pcm_Volume,
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

static const GDBusPropertyInfo bluealsa_iface_rfcomm_Transport = {
	-1, "Transport", "s", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_rfcomm_Features = {
	-1, "Features", "u", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo bluealsa_iface_rfcomm_Battery = {
	-1, "Battery", "y", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static const GDBusPropertyInfo *bluealsa_iface_rfcomm_properties[] = {
	&bluealsa_iface_rfcomm_Transport,
	&bluealsa_iface_rfcomm_Features,
	&bluealsa_iface_rfcomm_Battery,
	NULL,
};

const GDBusInterfaceInfo bluealsa_iface_manager = {
	-1, BLUEALSA_IFACE_MANAGER,
	NULL,
	NULL,
	(GDBusPropertyInfo **)bluealsa_iface_manager_properties,
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
