/*
 * BlueALSA - sco.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SCO_H_
#define BLUEALSA_SCO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-adapter.h"
#include "ba-transport.h"

int sco_setup_connection_dispatcher(struct ba_adapter *a);
int sco_transport_init(struct ba_transport *t);
int sco_transport_start(struct ba_transport *t);

#endif
