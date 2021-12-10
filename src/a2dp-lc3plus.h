/*
 * BlueALSA - a2dp-lc3plus.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_A2DPLC3PLUS_H_
#define BLUEALSA_A2DPLC3PLUS_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "a2dp.h"
#include "ba-transport.h"

extern struct a2dp_codec a2dp_lc3plus_sink;
extern struct a2dp_codec a2dp_lc3plus_source;

void a2dp_lc3plus_init(void);
void a2dp_lc3plus_transport_init(struct ba_transport *t);
int a2dp_lc3plus_transport_start(struct ba_transport *t);

#endif
