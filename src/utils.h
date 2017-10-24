/*
 * BlueALSA - utils.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_UTILS_H_
#define BLUEALSA_UTILS_H_

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdbool.h>
#include <time.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <gio/gio.h>

#include "bluez.h"

int a2dp_sbc_default_bitpool(int freq, int mode);

int hci_devlist(struct hci_dev_info **di, int *num);
int hci_open_sco(const struct hci_dev_info *di, const bdaddr_t *ba, bool transparent);

const char *bluetooth_profile_to_string(enum bluetooth_profile profile, uint16_t codec);
const char *batostr_(const bdaddr_t *ba);

const char *g_dbus_get_profile_object_path(enum bluetooth_profile profile, uint16_t codec);
enum bluetooth_profile g_dbus_object_path_to_profile(const char *path);
uint16_t g_dbus_object_path_to_a2dp_codec(const char *path);
int g_dbus_device_path_to_bdaddr(const char *path, bdaddr_t *addr);

GVariant *g_dbus_get_property(GDBusConnection *conn, const char *name,
		const char *path, const char *interface, const char *property);
gboolean g_dbus_set_property(GDBusConnection *conn, const char *name,
		const char *path, const char *interface, const char *property,
		const GVariant *value);

void snd_pcm_scale_s16le(int16_t *buffer, size_t size, int channels,
		double ch1_scale, double ch2_scale);

#if ENABLE_AAC
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/aacenc_lib.h>
const char *aacdec_strerror(AAC_DECODER_ERROR err);
const char *aacenc_strerror(AACENC_ERROR err);
#endif

#endif
