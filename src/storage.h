/*
 * BlueALSA - storage.h
 * SPDX-FileCopyrightText: 2022-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_STORAGE_H_
#define BLUEALSA_STORAGE_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-device.h"
#include "ba-transport-pcm.h"

int storage_init(const char *root);
void storage_destroy(void);

int storage_device_load(const struct ba_device *d);
int storage_device_save(const struct ba_device *d);
int storage_device_clear(const struct ba_device *d);

int storage_pcm_data_sync(struct ba_transport_pcm *pcm);
int storage_pcm_data_update(const struct ba_transport_pcm *pcm);

#endif
