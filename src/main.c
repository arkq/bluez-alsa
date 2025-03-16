/*
 * BlueALSA - main.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <getopt.h>
#include <libgen.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#if ENABLE_LDAC
# include <ldacBT.h>
#endif

#if ENABLE_LHDC
# include <lhdcBT.h>
# include <lhdcBT_dec.h>
#endif

#include "a2dp.h"
#include "a2dp-sbc.h"
#include "audio.h"
#include "ba-config.h"
#include "bluealsa-dbus.h"
#include "bluealsa-iface.h"
#include "bluez.h"
#include "codec-sbc.h"
#include "hfp.h"
#if ENABLE_OFONO
# include "ofono.h"
#endif
#include "storage.h"
#if ENABLE_UPOWER
# include "upower.h"
#endif
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/nv.h"

/* If glib does not support immediate return in case of bus
 * name being owned by some other connection (glib < 2.54),
 * fall back to a default behavior - enter waiting queue. */
#ifndef G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE
# define G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE \
	G_BUS_NAME_OWNER_FLAGS_NONE
#endif

static bool dbus_name_acquired = false;
static int retval = EXIT_SUCCESS;

static char *get_a2dp_codecs(enum a2dp_type type) {

	const char *strv[16 + 1] = { NULL };
	size_t n = 0;

	const struct a2dp_sep * a2dp_seps_tmp[16];
	struct a2dp_sep * const * seps = a2dp_seps;
	for (const struct a2dp_sep *sep = *seps; sep != NULL; sep = *++seps) {
		if (sep->config.type != type)
			continue;
		a2dp_seps_tmp[n] = sep;
		if (++n >= ARRAYSIZE(a2dp_seps_tmp))
			break;
	}

	/* Sort A2DP codecs before displaying them. */
	qsort(a2dp_seps_tmp, n, sizeof(*a2dp_seps_tmp),
			QSORT_COMPAR(a2dp_sep_ptr_cmp));

	for (size_t i = 0; i < n; i++)
		strv[i] = a2dp_codecs_codec_id_to_string(a2dp_seps_tmp[i]->config.codec_id);

	return g_strjoinv(", ", (char **)strv);
}

static char *get_hfp_codecs(void) {

	const char *strv[] = {
		hfp_codec_id_to_string(HFP_CODEC_CVSD),
#if ENABLE_MSBC
		hfp_codec_id_to_string(HFP_CODEC_MSBC),
#endif
#if ENABLE_LC3_SWB
		hfp_codec_id_to_string(HFP_CODEC_LC3_SWB),
#endif
		NULL,
	};

	return g_strjoinv(", ", (char **)strv);
}

static gboolean main_loop_exit_handler(void *userdata) {
	g_main_loop_quit((GMainLoop *)userdata);
	return G_SOURCE_REMOVE;
}

static void g_bus_name_acquired(GDBusConnection *conn, const char *name, void *userdata) {
	(void)conn;
	(void)name;
	(void)userdata;

	debug("Acquired D-Bus service name: %s", name);
	dbus_name_acquired = true;

	bluealsa_dbus_register();

	bluez_init();
#if ENABLE_OFONO
	ofono_init();
#endif
#if ENABLE_UPOWER
	upower_init();
#endif

}

static void g_bus_name_lost(GDBusConnection *conn, const char *name, void *userdata) {
	(void)conn;

	if (!dbus_name_acquired)
		error(
			"Couldn't acquire D-Bus name. Please check D-Bus configuration."
			" Requested name: %s", name);
	else
		error("Lost BlueALSA D-Bus name: %s", name);

	g_main_loop_quit((GMainLoop *)userdata);

	dbus_name_acquired = false;
	retval = EXIT_FAILURE;
}

int main(int argc, char **argv) {

	int opt;
	static const char *opts = "hVSB:i:p:c:";
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "syslog", no_argument, NULL, 'S' },
		{ "loglevel", required_argument, NULL, 23 },
		{ "dbus", required_argument, NULL, 'B' },
		{ "device", required_argument, NULL, 'i' },
		{ "profile", required_argument, NULL, 'p' },
		{ "codec", required_argument, NULL, 'c' },
		{ "all-codecs", no_argument, NULL, 25 },
		{ "initial-volume", required_argument, NULL, 17 },
		{ "keep-alive", required_argument, NULL, 8 },
		{ "io-rt-priority", required_argument, NULL, 3 },
		{ "disable-realtek-usb-fix", no_argument, NULL, 21 },
		{ "a2dp-force-mono", no_argument, NULL, 6 },
		{ "a2dp-force-audio-cd", no_argument, NULL, 7 },
		{ "sbc-quality", required_argument, NULL, 14 },
#if ENABLE_AAC
		{ "aac-afterburner", no_argument, NULL, 4 },
		{ "aac-bitrate", required_argument, NULL, 5 },
		{ "aac-latm-version", required_argument, NULL, 15 },
		{ "aac-true-bps", no_argument, NULL, 18 },
		{ "aac-vbr", no_argument, NULL, 19 },
#endif
#if ENABLE_LC3PLUS
		{ "lc3plus-bitrate", required_argument, NULL, 20 },
#endif
#if ENABLE_LDAC
		{ "ldac-abr", no_argument, NULL, 10 },
		{ "ldac-quality", required_argument, NULL, 11 },
#endif
#if ENABLE_LHDC
		// TODO: LLAC/V3/V4, bit depth, sample frequency, LLAC bitrate
		{ "lhdc-quality", required_argument, NULL, 24 },
#endif
#if ENABLE_MP3LAME
		{ "mp3-algorithm", required_argument, NULL, 12 },
		{ "mp3-vbr-quality", required_argument, NULL, 13 },
#endif
#if ENABLE_MIDI
		{ "midi-advertisement", no_argument, NULL, 22 },
#endif
		{ "xapl-resp-name", required_argument, NULL, 16 },
		{ 0, 0, 0, 0 },
	};

	static const struct {
		uint32_t codec_id;
		bool *ptr;
	} hfp_codecs[] = {
		{ HFP_CODEC_CVSD, &config.hfp.codecs.cvsd },
#if ENABLE_MSBC
		{ HFP_CODEC_MSBC, &config.hfp.codecs.msbc },
#endif
#if ENABLE_LC3_SWB
		{ HFP_CODEC_LC3_SWB, &config.hfp.codecs.lc3_swb },
#endif
	};

	bool syslog = false;
	char dbus_service[32] = BLUEALSA_SERVICE;

	/* Check if syslog forwarding has been enabled. This check has to be
	 * done before anything else, so we can log early stage warnings and
	 * errors. */
	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("Usage:\n"
					"  %s -p PROFILE [OPTION]...\n"
					"\nOptions:\n"
					"  -h, --help\t\t\tprint this help and exit\n"
					"  -V, --version\t\t\tprint version and exit\n"
					"  -S, --syslog\t\t\tsend output to syslog\n"
					"  --loglevel=LEVEL\t\tminimum message priority\n"
					"  -B, --dbus=NAME\t\tD-Bus service name suffix\n"
					"  -i, --device=hciX\t\tHCI device(s) to use\n"
					"  -p, --profile=NAME\t\tset enabled BT profiles\n"
					"  -c, --codec=NAME\t\tset enabled BT audio codecs\n"
					"  --all-codecs\t\t\tenable all available BT audio codecs\n"
					"  --initial-volume=NUM\t\tinitial volume level [0-100]\n"
					"  --keep-alive=SEC\t\tkeep Bluetooth transport alive\n"
					"  --io-rt-priority=NUM\t\treal-time priority for IO threads\n"
					"  --disable-realtek-usb-fix\tdisable fix for mSBC on Realtek USB\n"
					"  --a2dp-force-mono\t\ttry to force monophonic sound\n"
					"  --a2dp-force-audio-cd\t\ttry to force 44.1 kHz sampling\n"
					"  --sbc-quality=MODE\t\tset SBC encoder quality mode\n"
#if ENABLE_AAC
					"  --aac-afterburner\t\tenable FDK AAC afterburner\n"
					"  --aac-bitrate=BPS\t\tCBR bitrate or max peak for VBR\n"
					"  --aac-latm-version=NUM\tselect LATM syntax version\n"
					"  --aac-true-bps\t\tenable true bit-per-second bit rate\n"
					"  --aac-vbr\t\t\tprefer VBR mode over CBR mode\n"
#endif
#if ENABLE_LC3PLUS
					"  --lc3plus-bitrate=BPS\t\tset LC3plus encoder CBR bitrate\n"
#endif
#if ENABLE_LDAC
					"  --ldac-abr\t\t\tenable LDAC adaptive bit rate\n"
					"  --ldac-quality=MODE\t\tset LDAC encoder quality mode\n"
#endif
#if ENABLE_LHDC
					"  --lhdc-quality=MODE\t\tset LHDC encoder quality mode\n"
#endif
#if ENABLE_MP3LAME
					"  --mp3-algorithm=TYPE\t\tselect LAME encoder algorithm type\n"
					"  --mp3-vbr-quality=MODE\tset LAME encoder VBR quality mode\n"
#endif
#if ENABLE_MIDI
					"  --midi-advertisement\t\tenable LE advertisement for BLE-MIDI\n"
#endif
					"  --xapl-resp-name=NAME\t\tset product name used by XAPL\n"
					"\nAvailable BT profiles:\n"
					"  - a2dp-source\tAdvanced Audio Source (v1.4)\n"
					"  - a2dp-sink\tAdvanced Audio Sink (v1.4)\n"
#if ENABLE_OFONO
					"  - hfp-ofono\tHands-Free AG/HF handled by oFono\n"
#endif
					"  - hfp-ag\tHands-Free Audio Gateway (v1.9)\n"
					"  - hfp-hf\tHands-Free (v1.9)\n"
					"  - hsp-ag\tHeadset Audio Gateway (v1.2)\n"
					"  - hsp-hs\tHeadset (v1.2)\n"
#if ENABLE_MIDI
					"  - midi\tBluetooth LE MIDI (v1.0)\n"
#endif
					"\n"
					"Available BT audio codecs:\n"
					"  a2dp-source:\t%s\n"
					"  a2dp-sink:\t%s\n"
					"  hfp-*:\t%s\n"
					"",
					argv[0],
					get_a2dp_codecs(A2DP_SOURCE),
					get_a2dp_codecs(A2DP_SINK),
					get_hfp_codecs());
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'S' /* --syslog */ :
			syslog = true;
			break;
		}

	log_open(basename(argv[0]), syslog);

	if (ba_config_init() != 0) {
		error("Couldn't initialize configuration");
		return EXIT_FAILURE;
	}

	/* parse options */
	optind = 0; opterr = 1;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
		case 'V' /* --version */ :
		case 'S' /* --syslog */ :
			break;

		case 23 /* --loglevel=LEVEL */ : {

			static const nv_entry_t values[] = {
				{ "error", .v.ui = LOG_ERR },
				{ "warning", .v.ui = LOG_WARNING },
				{ "info", .v.ui = LOG_INFO },
#if DEBUG
				{ "debug", .v.ui = LOG_DEBUG },
#endif
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid loglevel {%s}: %s", nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			log_set_min_priority(entry->v.ui);
			break;
		}

		case 'B' /* --dbus=NAME */ :
			snprintf(dbus_service, sizeof(dbus_service), BLUEALSA_SERVICE ".%s", optarg);
			if (!g_dbus_is_name(dbus_service)) {
				error("Invalid BlueALSA D-Bus service name: %s", dbus_service);
				return EXIT_FAILURE;
			}
			break;

		case 'i' /* --device=HCI */ :
			g_array_append_val(config.hci_filter, optarg);
			break;

		case 'p' /* --profile=NAME */ : {

			static const struct {
				const char *name;
				bool *ptr;
			} map[] = {
				{ "a2dp-source", &config.profile.a2dp_source },
				{ "a2dp-sink", &config.profile.a2dp_sink },
#if ENABLE_OFONO
				{ "hfp-ofono", &config.profile.hfp_ofono },
#endif
				{ "hfp-hf", &config.profile.hfp_hf },
				{ "hfp-ag", &config.profile.hfp_ag },
				{ "hsp-hs", &config.profile.hsp_hs },
				{ "hsp-ag", &config.profile.hsp_ag },
#if ENABLE_MIDI
				{ "midi", &config.profile.midi },
#endif
			};

			bool matched = false;
			for (size_t i = 0; i < ARRAYSIZE(map); i++)
				if (strcasecmp(optarg, map[i].name) == 0) {
					*map[i].ptr = true;
					matched = true;
					break;
				}

			if (!matched) {
				error("Invalid BT profile name: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}

		case 'c' /* --codec=NAME */ : {

			bool enable = true;
			bool matched = false;
			if (optarg[0] == '+' || optarg[0] == '-') {
				enable = optarg[0] == '+' ? true : false;
				optarg++;
			}

			struct a2dp_sep * const * seps = a2dp_seps;
			uint32_t codec_id = a2dp_codecs_codec_id_from_string(optarg);
			for (struct a2dp_sep *sep = *seps; sep != NULL; sep = *++seps)
				if (sep->config.codec_id == codec_id) {
					sep->enabled = enable;
					matched = true;
				}

			codec_id = hfp_codec_id_from_string(optarg);
			for (size_t i = 0; i < ARRAYSIZE(hfp_codecs); i++)
				if (hfp_codecs[i].codec_id == codec_id) {
					*hfp_codecs[i].ptr = enable;
					matched = true;
				}

			if (!matched) {
				error("Invalid BT codec name: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}

		case 25 /* --all-codecs */ : {
			struct a2dp_sep * const * seps = a2dp_seps;
			for (struct a2dp_sep *sep = *seps; sep != NULL; sep = *++seps)
				sep->enabled = true;
			for (size_t i = 0; i < ARRAYSIZE(hfp_codecs); i++)
				*hfp_codecs[i].ptr = true;
			break;
		}

		case 17 /* --initial-volume=NUM */ : {
			unsigned int vol = atoi(optarg);
			if (vol > 100) {
				error("Invalid initial volume [0, 100]: %s", optarg);
				return EXIT_FAILURE;
			}
			double level = audio_loudness_to_decibel(1.0 * vol / 100);
			config.volume_init_level = MIN(MAX(level, -96.0), 96.0) * 100;
			break;
		}

		case 8 /* --keep-alive=SEC */ :
			config.keep_alive_time = atof(optarg) * 1000;
			break;

		case 3 /* --io-rt-priority=NUM */ :
			config.io_thread_rt_priority = atoi(optarg);
			const int min = sched_get_priority_min(SCHED_FIFO);
			const int max = sched_get_priority_max(SCHED_FIFO);
			if (config.io_thread_rt_priority < min || max < config.io_thread_rt_priority) {
				error("Invalid IO thread RT priority [%d, %d]: %s", min, max, optarg);
				return EXIT_FAILURE;
			}
			break;

		case 21 /* --disable-realtek-usb-fix */ :
			config.disable_realtek_usb_fix = true;
			break;

		case 6 /* --a2dp-force-mono */ :
			config.a2dp.force_mono = true;
			break;
		case 7 /* --a2dp-force-audio-cd */ :
			config.a2dp.force_44100 = true;
			break;

		case 14 /* --sbc-quality=MODE */ : {

			static const nv_entry_t values[] = {
				{ "low", .v.ui = SBC_QUALITY_LOW },
				{ "medium", .v.ui = SBC_QUALITY_MEDIUM },
				{ "high", .v.ui = SBC_QUALITY_HIGH },
				{ "xq", .v.ui = SBC_QUALITY_XQ },
				{ "xq+", .v.ui = SBC_QUALITY_XQPLUS },
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid SBC encoder quality mode {%s}: %s",
						nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			config.sbc_quality = entry->v.ui;
			break;
		}

#if ENABLE_AAC
		case 4 /* --aac-afterburner */ :
			config.aac_afterburner = true;
			break;
		case 5 /* --aac-bitrate=BPS */ :
			config.aac_bitrate = atoi(optarg);
			break;
		case 15 /* --aac-latm-version=NUM */ : {
			char *tmp;
			config.aac_latm_version = strtoul(optarg, &tmp, 10);
			if (config.aac_latm_version > 2 || optarg == tmp || *tmp != '\0') {
				error("Invalid LATM version {0, 1, 2}: %s", optarg);
				return EXIT_FAILURE;
			}
			break;
		}
		case 18 /* --aac-true-bps */ :
			config.aac_true_bps = true;
			break;
		case 19 /* --aac-vbr */ :
			config.aac_prefer_vbr = true;
			break;
#endif

#if ENABLE_LC3PLUS
		case 20 /* --lc3plus-bitrate=BPS */ :
			config.lc3plus_bitrate = atoi(optarg);
			break;
#endif

#if ENABLE_LDAC
		case 10 /* --ldac-abr */ :
			config.ldac_abr = true;
			break;
		case 11 /* --ldac-quality=MODE */ : {

			static const nv_entry_t values[] = {
				{ "mobile", .v.ui = LDACBT_EQMID_MQ },
				{ "standard", .v.ui = LDACBT_EQMID_SQ },
				{ "high", .v.ui = LDACBT_EQMID_HQ },
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid LDAC encoder quality mode {%s}: %s",
						nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			config.ldac_eqmid = entry->v.ui;
			break;
		}
#endif

#if ENABLE_LHDC
		case 24 /* --lhdc-quality=MODE */ : {

			static const nv_entry_t values[] = {
				{ "low0", .v.ui = LHDCBT_QUALITY_LOW0 },
				{ "low1", .v.ui = LHDCBT_QUALITY_LOW1 },
				{ "low2", .v.ui = LHDCBT_QUALITY_LOW2 },
				{ "low3", .v.ui = LHDCBT_QUALITY_LOW3 },
				{ "low4", .v.ui = LHDCBT_QUALITY_LOW4 },
				{ "low",  .v.ui = LHDCBT_QUALITY_LOW  },
				{ "mid",  .v.ui = LHDCBT_QUALITY_MID  },
				{ "high", .v.ui = LHDCBT_QUALITY_HIGH },
				{ "auto", .v.ui = LHDCBT_QUALITY_AUTO },
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid LHDC encoder quality mode {%s}: %s",
						nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			config.lhdc_eqmid = entry->v.ui;
			break;
		}
#endif

#if ENABLE_MP3LAME
		case 12 /* --mp3-algorithm=TYPE */ : {

			static const nv_entry_t values[] = {
				{ "fast", .v.ui = 7 },
				{ "cheap", .v.ui = 5 },
				{ "expensive", .v.ui = 2 },
				{ "best", .v.ui = 0 },
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid LAME encoder algorithm type {%s}: %s",
						nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			config.lame_quality = entry->v.ui;
			break;
		}

		case 13 /* --mp3-vbr-quality=MODE */ : {

			static const nv_entry_t values[] = {
				{ "low", .v.ui = 6 },
				{ "medium", .v.ui = 4 },
				{ "standard", .v.ui = 2 },
				{ "high", .v.ui = 1 },
				{ "extreme", .v.ui = 0 },
				{ 0 },
			};

			const nv_entry_t *entry;
			if ((entry = nv_find(values, optarg)) == NULL) {
				error("Invalid LAME VBR quality mode {%s}: %s",
						nv_join_names(values), optarg);
				return EXIT_FAILURE;
			}

			config.lame_vbr_quality = entry->v.ui;
			break;
		}
#endif

#if ENABLE_MIDI
		case 22 /* --midi-advertisement */ :
			config.midi.advertise = true;
			break;
#endif

		case 16 /* --xapl-resp-name=NAME */ :
			config.hfp.xapl_product_name = optarg;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	/* check whether at least one BT profile was enabled */
	if (!(config.profile.a2dp_source || config.profile.a2dp_sink ||
				config.profile.hfp_hf || config.profile.hfp_ag ||
				config.profile.hsp_hs || config.profile.hsp_ag ||
				config.profile.hfp_ofono || config.profile.midi)) {
		error("It is required to enabled at least one BT profile");
		fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
		return EXIT_FAILURE;
	}

#if ENABLE_OFONO
	if ((config.profile.hfp_ag || config.profile.hfp_hf) && config.profile.hfp_ofono) {
		info("Disabling native HFP support due to enabled oFono profile");
		config.profile.hfp_ag = false;
		config.profile.hfp_hf = false;
	}
#endif

	/* initialize random number generator */
	srandom(time(NULL));

	char *address;
	GError *err;

	err = NULL;
	address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if ((config.dbus = g_dbus_connection_new_for_address_sync(address,
					G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
					G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
					NULL, NULL, &err)) == NULL) {
		error("Couldn't obtain D-Bus connection: %s", err->message);
		return EXIT_FAILURE;
	}

#if ENABLE_OFONO
	/* Enabling native HFP support while oFono is running might interfere
	 * with oFono, so in the end neither BlueALSA nor oFono will work. */
	if ((config.profile.hfp_ag || config.profile.hfp_hf) && ofono_detect_service()) {
		warn("Disabling native HFP support due to oFono service presence");
		config.profile.hfp_ag = false;
		config.profile.hfp_hf = false;
	}
#endif

	/* Make sure that mandatory codecs are enabled. */
	a2dp_sbc_source.enabled = true;
	a2dp_sbc_sink.enabled = true;
	config.hfp.codecs.cvsd = true;

	if (a2dp_seps_init() == -1)
		return EXIT_FAILURE;

	const char *storage_base_dir = BLUEALSA_STORAGE_DIR;
#if ENABLE_SYSTEMD
	const char *systemd_state_dir;
	if ((systemd_state_dir = getenv("STATE_DIRECTORY")) != NULL)
		storage_base_dir = systemd_state_dir;
#endif
	storage_init(storage_base_dir);

	/* In order to receive EPIPE while writing to the pipe whose reading end
	 * is closed, the SIGPIPE signal has to be handled. For more information
	 * see the io_thread_write_pcm() function. */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, main_loop_exit_handler, loop);
	g_unix_signal_add(SIGTERM, main_loop_exit_handler, loop);

	/* register well-known service name */
	g_bus_own_name_on_connection(config.dbus,
			dbus_service, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
			g_bus_name_acquired, g_bus_name_lost, loop, NULL);

	/* main dispatching loop */
	debug("Starting main dispatching loop");
	g_main_loop_run(loop);

	/* cleanup internal structures */
	bluez_destroy();

	storage_destroy();
	g_dbus_connection_close_sync(config.dbus, NULL, NULL);
	g_main_loop_unref(loop);
	g_free(address);

	return retval;
}
