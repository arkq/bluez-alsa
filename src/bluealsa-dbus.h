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

#include <gio/gio.h>
#include <glib.h>

#include "ba-transport.h"

int bluealsa_dbus_register_manager(GError **error);

int bluealsa_dbus_register_transport(struct ba_transport *transport);
void bluealsa_dbus_unregister_transport(struct ba_transport *transport);

#endif
