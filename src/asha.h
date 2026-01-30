/*
 * BlueALSA - asha.h
 * SPDX-FileCopyrightText: 2025-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_ASHA_H_
#define BLUEALSA_ASHA_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>

#include "ba-transport.h"

uint8_t asha_codec_id_from_string(const char * alias);
const char * asha_codec_id_to_string(uint8_t codec_id);
int asha_transport_start(struct ba_transport * t);

#endif
