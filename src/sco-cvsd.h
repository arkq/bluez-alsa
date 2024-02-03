/*
 * BlueALSA - sco-cvsd.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SCOCVSD_H_
#define BLUEALSA_SCOCVSD_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-transport-pcm.h"

void *sco_cvsd_enc_thread(struct ba_transport_pcm *t_pcm);
void *sco_cvsd_dec_thread(struct ba_transport_pcm *t_pcm);

#endif
