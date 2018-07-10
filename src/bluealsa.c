/*
 * BlueALSA - bluealsa.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa.h"

#include <grp.h>

#include "hfp.h"
#include "transport.h"


/* Initialize global configuration variable. */
struct ba_config config = {

	/* enable output profiles by default */
	.enable.a2dp_source = true,
	.enable.hsp_ag = true,
	.enable.hfp_ag = true,

	/* initialization flags */
	.ctl.socket_created = false,
	.ctl.thread_created = false,

	/* omit chown if audio group is not defined */
	.gid_audio = -1,

	.hfp.features_sdp_hf =
		SDP_HFP_HF_FEAT_CLI |
		SDP_HFP_HF_FEAT_VOLUME,
	.hfp.features_sdp_ag = 0,
	.hfp.features_rfcomm_hf =
		HFP_HF_FEAT_CLI |
		HFP_HF_FEAT_VOLUME |
		HFP_HF_FEAT_ECS |
		HFP_HF_FEAT_ECC |
		HFP_HF_FEAT_CODEC,
	.hfp.features_rfcomm_ag =
		HFP_AG_FEAT_REJECT |
		HFP_AG_FEAT_ECS |
		HFP_AG_FEAT_ECC |
		HFP_AG_FEAT_EERC |
		HFP_AG_FEAT_CODEC,

	.a2dp.volume = false,
	.a2dp.force_mono = false,
	.a2dp.force_44100 = false,

#if ENABLE_AAC
	/* There are two issues with the afterburner: a) it uses a LOT of power,
	 * b) it generates larger payload (see VBR comment). These two reasons
	 * are good enough to not enable afterburner by default. */
	.aac_afterburner = false,
	.aac_vbr_mode = 4,
#endif

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

	config.a2dp.codecs = bluez_a2dp_codecs;

	return 0;
}

void bluealsa_config_free(void) {
	pthread_mutex_destroy(&config.devices_mutex);
	g_hash_table_unref(config.devices);
	g_hash_table_unref(config.dbus_objects);
}
