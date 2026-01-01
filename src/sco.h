/*
 * BlueALSA - sco.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_SCO_H_
#define BLUEALSA_SCO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-adapter.h"
#include "ba-transport.h"
#include "error.h"

error_code_t sco_setup_connection_dispatcher(struct ba_adapter * a);
int sco_transport_init(struct ba_transport * t);
int sco_transport_start(struct ba_transport * t);

#endif
