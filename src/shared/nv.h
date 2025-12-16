/*
 * BlueALSA - nv.h
 * SPDX-FileCopyrightText: 2022-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_SHARED_NV_H_
#define BLUEALSA_SHARED_NV_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

/**
 * A single entry of name:value array. */
typedef struct nv_entry {
	/* the name of the entry */
	const char * name;
	/* associated value */
	union {
		int i;
		unsigned int u;
	} v;
} nv_entry_t;

nv_entry_t * nv_lookup_entry(const nv_entry_t * entries, const char * name);
const char * nv_name_from_int(const nv_entry_t * entries, int value);
const char * nv_name_from_uint(const nv_entry_t * entries, unsigned int value);
char * nv_join_names(const nv_entry_t * entries);

#endif
