/*
 * BlueALSA - utils.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_UTILS_H_
#define BLUEALSA_UTILS_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>

#include <glib.h>

int g_dbus_bluez_object_path_to_hci_dev_id(const char *path);
bdaddr_t *g_dbus_bluez_object_path_to_bdaddr(const char *path, bdaddr_t *addr);

char *g_variant_sanitize_object_path(char *path);
bool g_variant_validate_value(GVariant *value, const GVariantType *type,
		const char *name);

GSource *g_io_create_watch_full(GIOChannel *channel, int priority,
		GIOCondition cond, GIOFunc func, void *userdata, GDestroyNotify notify);

unsigned int g_bdaddr_hash(const void *v);
gboolean g_bdaddr_equal(const void *v1, const void *v2);

#if ENABLE_MP3LAME
int a2dp_mpeg1_mp3_get_max_bitrate(uint16_t mask);
#endif

#endif
