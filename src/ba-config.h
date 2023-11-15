/*
 * BlueALSA - ba-config.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BACONFIG_H_
#define BLUEALSA_BACONFIG_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h> /* IWYU pragma: keep */
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <glib.h>

struct ba_config {

	/* set of enabled profiles */
	struct {
		bool a2dp_source;
		bool a2dp_sink;
		bool hfp_ofono;
		bool hfp_hf;
		bool hfp_ag;
		bool hsp_hs;
		bool hsp_ag;
		bool midi;
	} profile;

	/* established D-Bus connection */
	GDBusConnection *dbus;

	/* adapters indexed by the HCI device ID */
	pthread_mutex_t adapters_mutex;
	struct ba_adapter *adapters[HCI_MAX_DEV];

	/* List of HCI names (or BT addresses) used for adapters filtering
	 * during profile registration. Leave it empty to use any adapter. */
	GArray *hci_filter;

	/* device connection sequence number */
	atomic_uint device_seq;

	/* used for main thread identification */
	pthread_t main_thread;

	/* opened null device */
	int null_fd;

	/* The number of milliseconds for keeping BT transport alive after
	 * PCM has been closed. One might set this value to negative number for
	 * infinite time. This option applies for the source profile only. */
	int keep_alive_time;

	/* real-time scheduling priority of transport IO threads */
	int io_thread_rt_priority;

	/* the initial volume level */
	int volume_init_level;

	/* disable alt-3 MTU for mSBC with Realtek USB adapters */
	bool disable_realtek_usb_fix;

	struct {

		/* available HFP codecs */
		struct {
			bool cvsd;
#if ENABLE_MSBC
			bool msbc;
#endif
#if ENABLE_LC3_SWB
			bool lc3_swb;
#endif
		} codecs;

		/* information exposed via Apple AT extension */
		unsigned int xapl_vendor_id;
		unsigned int xapl_product_id;
		unsigned int xapl_sw_version;
		const char *xapl_product_name;
		unsigned int xapl_features;

	} hfp;

	struct {
		bool available;
		/* host battery level (percentage) */
		unsigned int level;
	} battery;

	struct {

		/* NULL-terminated list of available A2DP codecs */
		const struct bluez_a2dp_codec **codecs;

		/* Control audio volume natively by the connected device. The disadvantage
		 * of this control type is a monophonic volume change. */
		bool volume;

		/* Support for monophonic sound in the A2DP profile is mandatory for
		 * sink and semi-mandatory for source. So, if one wants only the bare
		 * minimum, it would be possible - e.g. due to bandwidth limitations. */
		bool force_mono;
		/* The sample rates of 44.1 kHz (aka Audio CD) and 48 kHz are mandatory
		 * for sink endpoint and semi-mandatory for source. It is then possible
		 * to force lower sampling in order to save Bluetooth bandwidth. */
		bool force_44100;

	} a2dp;

#if ENABLE_MIDI
	struct {
		/* advertise BLE-MIDI via LE advertisement */
		bool advertise;
	} midi;
#endif

	/* BlueALSA supports 5 SBC qualities: low, medium, high, XQ and XQ+. The XQ
	 * mode uses 44.1 kHz sample rate, dual channel mode with bitpool 38, 16
	 * blocks in frame, 8 frequency bands and allocation method Loudness, which
	 * is also known as SBC XQ Dual Channel HD. The "+" version uses bitpool 47
	 * instead of 38. */
	uint8_t sbc_quality;

#if ENABLE_AAC
	bool aac_afterburner;
	bool aac_prefer_vbr;
	bool aac_true_bps;
	unsigned int aac_bitrate;
	unsigned int aac_latm_version;
#endif

#if ENABLE_MP3LAME
	uint8_t lame_quality;
	uint8_t lame_vbr_quality;
#endif

#if ENABLE_LC3PLUS
	unsigned int lc3plus_bitrate;
#endif

#if ENABLE_LDAC
	bool ldac_abr;
	uint8_t ldac_eqmid;
#endif

#if ENABLE_LHDC
	uint8_t lhdc_eqmid;
	// TODO: LLAC/V3/V4, bit depth, sample frequency, LLAC bitrate
#endif
};

/* Global BlueALSA configuration. */
extern struct ba_config config;

int ba_config_init(void);

unsigned int ba_config_get_hfp_sdp_features_ag(void);
unsigned int ba_config_get_hfp_sdp_features_hf(void);

#endif
