/*
 * BlueALSA - utils.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_UTILS_H_
#define BLUEALSA_UTILS_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <time.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <gio/gio.h>

#include "ba-transport.h"

int a2dp_sbc_default_bitpool(int freq, int mode);

int hci_devlist(struct hci_dev_info **di, int *num);
int hci_open_sco(const struct hci_dev_info *di, const bdaddr_t *ba, bool transparent);
const char *batostr_(const bdaddr_t *ba);

bdaddr_t *g_dbus_bluez_object_path_to_bdaddr(const char *path, bdaddr_t *addr);
struct ba_transport_type g_dbus_bluez_object_path_to_transport_type(const char *path);
const char *g_dbus_transport_type_to_bluez_object_path(struct ba_transport_type type);

GVariant *g_dbus_get_property(GDBusConnection *conn, const char *name,
		const char *path, const char *interface, const char *property);
gboolean g_dbus_set_property(GDBusConnection *conn, const char *name,
		const char *path, const char *interface, const char *property,
		const GVariant *value);

void snd_pcm_scale_s16le(int16_t *buffer, size_t size, int channels,
		double ch1_scale, double ch2_scale);

const char *bluetooth_a2dp_codec_to_string(uint16_t codec);
const char *ba_transport_type_to_string(struct ba_transport_type type);

#if ENABLE_AAC
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/aacenc_lib.h>
const char *aacdec_strerror(AAC_DECODER_ERROR err);
const char *aacenc_strerror(AACENC_ERROR err);
#endif

#if ENABLE_LDAC
const char *ldacBT_strerror(int err);
#endif

#endif
