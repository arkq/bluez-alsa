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
#include <stdint.h>
#include <time.h>

#include <bluetooth/bluetooth.h>

#include <gio/gio.h>
#include <glib.h>

#include "ba-transport.h"

int a2dp_sbc_default_bitpool(int freq, int mode);

int hci_open_sco(int dev_id, const bdaddr_t *ba, bool transparent);
const char *batostr_(const bdaddr_t *ba);

int g_dbus_bluez_object_path_to_hci_dev_id(const char *path);
bdaddr_t *g_dbus_bluez_object_path_to_bdaddr(const char *path, bdaddr_t *addr);
const char *g_dbus_transport_type_to_bluez_object_path(struct ba_transport_type type);
uint16_t g_dbus_transport_get_pcm_format(const struct ba_transport_type type);

GVariantIter *g_dbus_get_managed_objects(GDBusConnection *conn,
		const char *name, const char *path, GError **error);
GVariant *g_dbus_get_property(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, const char *property,
		GError **error);
bool g_dbus_set_property(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, const char *property,
		const GVariant *value, GError **error);

char *g_variant_sanitize_object_path(char *path);

void snd_pcm_scale_s16le(int16_t *buffer, size_t size, int channels,
		double ch1_scale, double ch2_scale);

const char *bluetooth_a2dp_codec_to_string(uint16_t codec);
const char *ba_transport_type_to_string(struct ba_transport_type type);

#if ENABLE_MP3LAME
int a2dp_mpeg1_mp3_get_max_bitrate(uint16_t mask);
const char *lame_encode_strerror(int err);
#endif

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
