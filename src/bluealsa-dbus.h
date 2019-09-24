/*
 * BlueALSA - bluealsa-dbus.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
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

#include "ba-transport.h"

#define BA_DBUS_TRANSPORT_UPDATE_CHANNELS (1 << 0)
#define BA_DBUS_TRANSPORT_UPDATE_SAMPLING (1 << 1)
#define BA_DBUS_TRANSPORT_UPDATE_CODEC    (1 << 2)
#define BA_DBUS_TRANSPORT_UPDATE_DELAY    (1 << 3)
#define BA_DBUS_TRANSPORT_UPDATE_VOLUME   (1 << 4)
#define BA_DBUS_TRANSPORT_UPDATE_BATTERY  (1 << 5)
#define BA_DBUS_TRANSPORT_UPDATE_FORMAT   (1 << 6)

int bluealsa_dbus_manager_register(GError **error);

int bluealsa_dbus_transport_register(struct ba_transport *t, GError **error);
void bluealsa_dbus_transport_update(struct ba_transport *t, unsigned int mask);
void bluealsa_dbus_transport_unregister(struct ba_transport *t);

#endif
