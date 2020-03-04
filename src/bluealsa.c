/*
 * BlueALSA - bluealsa.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa.h"

#include <fcntl.h>
#include <stdbool.h>

#if ENABLE_LDAC
# include <ldacBT.h>
#endif

#include "bluez-a2dp.h"
#include "hfp.h"
#include "sbc.h"

/* Initialize global configuration variable. */
struct ba_config config = {

	/* enable output profiles by default */
	.enable.a2dp_source = true,
	.enable.hfp_ag = true,
	.enable.hsp_ag = true,

	.adapters_mutex = PTHREAD_MUTEX_INITIALIZER,

	.null_fd = -1,

	.hfp.features_sdp_hf =
		SDP_HFP_HF_FEAT_CLI |
		SDP_HFP_HF_FEAT_VOLUME |
#if ENABLE_MSBC
		SDP_HFP_HF_FEAT_WBAND |
#endif
		0,
	.hfp.features_sdp_ag =
#if ENABLE_MSBC
		SDP_HFP_AG_FEAT_WBAND |
#endif
		0,
	.hfp.features_rfcomm_hf =
		HFP_HF_FEAT_CLI |
		HFP_HF_FEAT_VOLUME |
		HFP_HF_FEAT_ECS |
		HFP_HF_FEAT_ECC |
		0,
	.hfp.features_rfcomm_ag =
		HFP_AG_FEAT_REJECT |
		HFP_AG_FEAT_ECS |
		HFP_AG_FEAT_ECC |
		0,

	/* build-in Apple accessory identification */
	.hfp.xapl_vendor_id = 0xB103,
	.hfp.xapl_product_id = 0xA15A,
	.hfp.xapl_software_version = "0200",
	.hfp.xapl_features =
		XAPL_FEATURE_BATTERY |
		XAPL_FEATURE_DOCKING |
		0,

	/* Initially set host battery as unavailable. If UPower integration was
	 * enabled, this value will be automatically updated via D-Bus event. */
	.battery.available = false,
	.battery.level = 100,

	.a2dp.volume = false,
	.a2dp.force_mono = false,
	.a2dp.force_44100 = false,
	.a2dp.keep_alive = 0,

	/* Try to use high SBC encoding quality as a default. */
	.sbc_quality = SBC_QUALITY_HIGH,

#if ENABLE_AAC
	/* There are two issues with the afterburner: a) it uses a LOT of power,
	 * b) it generates larger payload. These two reasons are good enough to
	 * not enable afterburner by default. */
	.aac_afterburner = false,
	.aac_vbr_mode = 4,
#endif

#if ENABLE_MP3LAME
	.lame_quality = 5,
	/* Use high quality for VBR mode (~190 kbps) as a default. */
	.lame_vbr_quality = 2,
#endif

#if ENABLE_LDAC
	.ldac_abr = false,
	/* Use standard encoder quality as a reasonable default. */
	.ldac_eqmid = LDACBT_EQMID_SQ,
#endif

};

int bluealsa_config_init(void) {

	config.hci_filter = g_array_sized_new(FALSE, FALSE, sizeof(const char *), 4);

	config.main_thread = pthread_self();

	config.null_fd = open("/dev/null", O_WRONLY | O_NONBLOCK);

	config.a2dp.codecs = bluez_a2dp_codecs;

	return 0;
}
