/*
 * BlueALSA - bluealsa-config.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-config.h"

#include <fcntl.h>
#include <stdbool.h>

#if ENABLE_LDAC
# include <ldacBT.h>
#endif

#include "codec-sbc.h"
#include "hfp.h"

/* Initialize global configuration variable. */
struct ba_config config = {

	/* enable output profiles by default */
	.profile.a2dp_source = true,
	.profile.hfp_ag = true,
	.profile.hsp_ag = true,

	.adapters_mutex = PTHREAD_MUTEX_INITIALIZER,

	.device_seq = 0,

	.null_fd = -1,

	.keep_alive_time = 0,

	.volume_init_level = 0,

	/* CVSD is a mandatory codec */
	.hfp.codecs.cvsd = true,
#if ENABLE_MSBC
	/* mSBC is an optional codec but provides better audio
	 * quality, so we will enable it by default */
	.hfp.codecs.msbc = true,
#endif

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
	.hfp.xapl_software_version = "0300",
	.hfp.xapl_product_name = "BlueALSA",
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

	/* Try to use high SBC encoding quality as a default. */
	.sbc_quality = SBC_QUALITY_HIGH,

#if ENABLE_AAC
	/* There are two issues with the afterburner: a) it uses a LOT of power,
	 * b) it generates larger payload. These two reasons are good enough to
	 * not enable afterburner by default. */
	.aac_afterburner = false,
	/* By default try not to not use the VBR mode. Please note, that for A2DP
	 * sink the VBR mode might still be used if the connection is initialized
	 * by a remote BT device. */
	.aac_prefer_vbr = false,
	/* Do not enable true "bit per second" bit rate by default because it
	 * violates A2DP AAC specification. */
	.aac_true_bps = false,
	/* For CBR mode the 220 kbps bitrate should result in an A2DP frame equal
	 * to 651 bytes. Such frame size should fit within writing MTU of most BT
	 * headsets, so it will prevent RTP fragmentation which seems not to be
	 * supported by every BT headset out there. */
	.aac_bitrate = 220000,
	/* Use newer LATM syntax by default. Please note, that some older BT
	 * devices might not understand this syntax, so for them it might be
	 * required to use LATM version 0 (ISO-IEC 14496-3 (2001)). */
	.aac_latm_version = 1,
#endif

#if ENABLE_MP3LAME
	.lame_quality = 5,
	/* Use high quality for VBR mode (~190 kbps) as a default. */
	.lame_vbr_quality = 2,
#endif

#if ENABLE_LC3PLUS
	/* Set default bitrate to 396.8 kbps. Such value should result in a high
	 * quality with a guarantee that LC3plus frames will not be fragmented. */
	.lc3plus_bitrate = 396800,
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

	return 0;
}
