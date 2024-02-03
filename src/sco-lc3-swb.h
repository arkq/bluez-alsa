/*
 * BlueALSA - sco-lc3-swb.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SCOLC3SWB_H_
#define BLUEALSA_SCOLC3SWB_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-transport-pcm.h"

void *sco_lc3_swb_enc_thread(struct ba_transport_pcm *t_pcm);
void *sco_lc3_swb_dec_thread(struct ba_transport_pcm *t_pcm);

#endif
