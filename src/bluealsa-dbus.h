/*
 * BlueALSA - bluealsa-dbus.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEALSADBUS_H_
#define BLUEALSA_BLUEALSADBUS_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <glib.h>

#include "ba-rfcomm.h"
#include "ba-transport.h"

#define BA_DBUS_PCM_UPDATE_FORMAT      (1 << 0)
#define BA_DBUS_PCM_UPDATE_CHANNELS    (1 << 1)
#define BA_DBUS_PCM_UPDATE_SAMPLING    (1 << 2)
#define BA_DBUS_PCM_UPDATE_CODEC       (1 << 3)
#define BA_DBUS_PCM_UPDATE_DELAY       (1 << 4)
#define BA_DBUS_PCM_UPDATE_SOFT_VOLUME (1 << 5)
#define BA_DBUS_PCM_UPDATE_VOLUME      (1 << 6)

#define BA_DBUS_RFCOMM_UPDATE_FEATURES (1 << 0)
#define BA_DBUS_RFCOMM_UPDATE_BATTERY  (1 << 1)

unsigned int bluealsa_dbus_manager_register(GError **error);

unsigned int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm, GError **error);
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask);
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm);

unsigned int bluealsa_dbus_rfcomm_register(struct ba_rfcomm *r, GError **error);
void bluealsa_dbus_rfcomm_update(struct ba_rfcomm *r, unsigned int mask);
void bluealsa_dbus_rfcomm_unregister(struct ba_rfcomm *r);

#endif
