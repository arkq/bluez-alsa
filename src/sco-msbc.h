/*
 * BlueALSA - sco-msbc.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_SCOMSBC_H_
#define BLUEALSA_SCOMSBC_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-transport-pcm.h"

void *sco_msbc_enc_thread(struct ba_transport_pcm *t_pcm);
void *sco_msbc_dec_thread(struct ba_transport_pcm *t_pcm);

#endif
