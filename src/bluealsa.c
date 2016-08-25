/*
 * BlueALSA - bluealsa.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa.h"

#include "transport.h"


int bluealsa_setup_init(struct ba_setup *setup) {

	setup->enable_a2dp = TRUE;
	setup->enable_hsp = TRUE;

	pthread_mutex_init(&setup->devices_mutex, NULL);
	setup->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)device_free);

	setup->dbus_objects = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

	return 0;
}

void bluealsa_setup_free(struct ba_setup *setup) {
	g_hash_table_unref(setup->devices);
	g_hash_table_unref(setup->dbus_objects);
}
