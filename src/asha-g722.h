/*
 * BlueALSA - asha-g722.h
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_ASHAG722_H_
#define BLUEALSA_ASHAG722_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-transport-pcm.h"

void * asha_g722_enc_thread(struct ba_transport_pcm * t_pcm);
void * asha_g722_dec_thread(struct ba_transport_pcm * t_pcm);

#endif
