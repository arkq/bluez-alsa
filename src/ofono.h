/*
 * BlueALSA - ofono.h
 * SPDX-FileCopyrightText: 2018-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_OFONO_H_
#define BLUEALSA_OFONO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>

#include "ba-transport.h"

int ofono_init(void);
bool ofono_detect_service(void);

int ofono_call_volume_update(struct ba_transport *t);

#endif
