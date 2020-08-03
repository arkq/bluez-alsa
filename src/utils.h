/*
 * BlueALSA - utils.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
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

#include <bluetooth/bluetooth.h>

#include <glib.h>

#include "ba-transport.h"

int g_dbus_bluez_object_path_to_hci_dev_id(const char *path);
bdaddr_t *g_dbus_bluez_object_path_to_bdaddr(const char *path, bdaddr_t *addr);
const char *g_dbus_transport_type_to_bluez_object_path(struct ba_transport_type type);

char *g_variant_sanitize_object_path(char *path);
bool g_variant_validate_value(GVariant *value, const GVariantType *type,
		const char *name);

unsigned int g_bdaddr_hash(const void *v);
gboolean g_bdaddr_equal(const void *v1, const void *v2);

uint16_t ba_transport_codecs_a2dp_from_string(const char *str);
const char *ba_transport_codecs_a2dp_to_string(uint16_t codec);
uint16_t ba_transport_codecs_hfp_from_string(const char *str);
const char *ba_transport_codecs_hfp_to_string(uint16_t codec);

const char *ba_transport_type_to_string(struct ba_transport_type type);

#if ENABLE_MP3LAME
int a2dp_mpeg1_mp3_get_max_bitrate(uint16_t mask);
const char *lame_encode_strerror(int err);
#endif

#if ENABLE_AAC
# include <fdk-aac/aacdecoder_lib.h>
# include <fdk-aac/aacenc_lib.h>
const char *aacdec_strerror(AAC_DECODER_ERROR err);
const char *aacenc_strerror(AACENC_ERROR err);
#endif

#if ENABLE_LDAC
const char *ldacBT_strerror(int err);
#endif

#endif
