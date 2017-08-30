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
#include <sys/eventfd.h>

#include "transport.h"


/* Initialize global configuration variable. */
struct ba_config config = {

	/* by default all profiles are enabled */
	.enable_a2dp = true,
	.enable_hsp = true,
	.enable_hfp = true,

	/* initialization flags */
	.ctl.socket_created = false,
	.ctl.thread_created = false,

	/* omit chown if audio group is not defined */
	.gid_audio = -1,

#if ENABLE_AAC
	/* There are two issues with the afterburner: a) it uses a LOT of power,
	 * b) it generates larger payload (see VBR comment). These two reasons
	 * are good enough to not enable afterburner by default. */
	.aac_afterburner = false,
	/* Low bitrate for VBR mode should ensure, that the RTP payload will not
	 * exceed our writing MTU. It is important not to do so, because the code
	 * responsible for fragmentation seems not to work as expected. */
	.aac_vbr_mode = 3,
#endif

	.a2dp_force_mono = false,
	.a2dp_force_44100 = false,
	.a2dp_volume = false,

};

int bluealsa_config_init(void) {

	struct group *grp;

	config.main_thread = pthread_self();

	pthread_mutex_init(&config.devices_mutex, NULL);
	config.devices = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)device_free);

	config.dbus_objects = g_hash_table_new_full(g_direct_hash, g_direct_equal,
			NULL, g_free);

	/* use proper ACL group for our audio device */
	if ((grp = getgrnam("audio")) != NULL)
		config.gid_audio = grp->gr_gid;

	return 0;
}

void bluealsa_config_free(void) {
	pthread_mutex_destroy(&config.devices_mutex);
	g_hash_table_unref(config.devices);
	g_hash_table_unref(config.dbus_objects);
}

void bluealsa_event() {
	eventfd_write(config.ctl.pfds[CTL_IDX_EVT].fd, 1);
}
