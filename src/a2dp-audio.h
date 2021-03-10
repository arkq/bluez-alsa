/*
 * BlueALSA - a2dp-audio.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_A2DPAUDIO_H_
#define BLUEALSA_A2DPAUDIO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-transport.h"

int a2dp_audio_thread_create(struct ba_transport *t);

#endif
