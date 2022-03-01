/*
 * BlueALSA - nv.h
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
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
	const char *name;
	/* associated value */
	union {
		int i;
		unsigned int ui;
	} v;
} nv_entry_t;

nv_entry_t *nv_find(const nv_entry_t *entries, const char *name);
char *nv_join_names(const nv_entry_t *entries);

#endif
