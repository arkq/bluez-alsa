/*
 * BlueALSA - a2dp-lhdc.h
 * SPDX-FileCopyrightText: 2023-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_A2DPLHDC_H_
#define BLUEALSA_A2DPLHDC_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "a2dp.h"

extern struct a2dp_sep a2dp_lhdc_v2_source;
extern struct a2dp_sep a2dp_lhdc_v2_sink;

extern struct a2dp_sep a2dp_lhdc_v3_source;
extern struct a2dp_sep a2dp_lhdc_v3_sink;

extern struct a2dp_sep a2dp_lhdc_v5_source;
extern struct a2dp_sep a2dp_lhdc_v5_sink;

#endif
