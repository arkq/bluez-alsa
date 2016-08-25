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


int bluealsa_setup_init(struct ba_setup *setup) {

	setup->enable_a2dp = TRUE;
	setup->enable_hsp = TRUE;

	pthread_mutex_init(&setup->devices_mutex, NULL);
	setup->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)device_free);

	setup->dbus_objects = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

	struct group *grp;
	setup->gid_audio = -1;

	/* use proper ACL group for our audio device */
	if ((grp = getgrnam("audio")) != NULL)
		setup->gid_audio = grp->gr_gid;

	setup->ctl_socket_created = FALSE;
	setup->ctl_thread_created = FALSE;

	return 0;
}

void bluealsa_setup_free(struct ba_setup *setup) {
	g_hash_table_unref(setup->devices);
	g_hash_table_unref(setup->dbus_objects);
}
