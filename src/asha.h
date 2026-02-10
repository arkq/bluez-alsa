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

#include "ba-transport.h"

int asha_transport_start(struct ba_transport * t);

#endif
