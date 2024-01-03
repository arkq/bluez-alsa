/*
 * BlueALSA - a2dp-ldac.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_A2DPLDAC_H_
#define BLUEALSA_A2DPLDAC_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "a2dp.h"
#include "ba-transport.h"

extern struct a2dp_codec a2dp_ldac_sink;
extern struct a2dp_codec a2dp_ldac_source;

void a2dp_ldac_transport_init(struct ba_transport *t);
int a2dp_ldac_transport_start(struct ba_transport *t);

#endif
