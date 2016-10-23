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

#include <grp.h>

#include "transport.h"


int bluealsa_config_init(void) {

	config.enable_a2dp = TRUE;
	config.enable_hsp = TRUE;

	pthread_mutex_init(&config.devices_mutex, NULL);
	config.devices = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)device_free);

	config.dbus_objects = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

	struct group *grp;
	config.gid_audio = -1;

	/* use proper ACL group for our audio device */
	if ((grp = getgrnam("audio")) != NULL)
		config.gid_audio = grp->gr_gid;

	config.ctl_socket_created = FALSE;
	config.ctl_thread_created = FALSE;

	return 0;
}

void bluealsa_config_free(void) {
	pthread_mutex_destroy(&config.devices_mutex);
	g_hash_table_unref(config.devices);
	g_hash_table_unref(config.dbus_objects);
}
