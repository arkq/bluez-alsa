/*
 * BlueALSA - a2dp-aac.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_A2DPAAC_H_
#define BLUEALSA_A2DPAAC_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "a2dp.h"
#include "ba-transport.h"

extern struct a2dp_codec a2dp_aac_sink;
extern struct a2dp_codec a2dp_aac_source;

void a2dp_aac_init(void);
void a2dp_aac_transport_init(struct ba_transport *t);
int a2dp_aac_transport_start(struct ba_transport *t);

#endif
