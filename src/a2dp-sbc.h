/*
 * BlueALSA - a2dp-sbc.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_A2DPSBC_H_
#define BLUEALSA_A2DPSBC_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "a2dp.h"

extern struct a2dp_sep a2dp_sbc_source;
extern struct a2dp_sep a2dp_sbc_sink;

#endif
