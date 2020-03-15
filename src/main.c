/*
 * BlueALSA - main.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <gio/gio.h>
#include <glib.h>

#if ENABLE_LDAC
# include <ldacBT.h>
#endif

#include "bluealsa.h"
#include "bluealsa-dbus.h"
#include "bluealsa-iface.h"
#include "bluez-a2dp.h"
#include "bluez.h"
#if ENABLE_OFONO
# include "ofono.h"
#endif
#include "sbc.h"
#include "utils.h"
#if ENABLE_UPOWER
# include "upower.h"
#endif
#include "shared/defs.h"
#include "shared/log.h"

/* If glib does not support immediate return in case of bus
 * name being owned by some other connection (glib < 2.54),
 * fall back to a default behavior - enter waiting queue. */
#ifndef G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE
# define G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE \
	G_BUS_NAME_OWNER_FLAGS_NONE
#endif

static GMainLoop *loop = NULL;
static int retval = EXIT_SUCCESS;

static char *get_a2dp_codecs(
		const struct bluez_a2dp_codec **codecs,
		enum bluez_a2dp_dir dir) {

	const char *tmp[16] = { NULL };
	int i = 0;

	while (*codecs != NULL) {
		const struct bluez_a2dp_codec *c = *codecs++;
		if (c->dir != dir)
			continue;
		tmp[i++] = bluetooth_a2dp_codec_to_string(c->id);
	}

	return g_strjoinv(", ", (char **)tmp);
}

static void main_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);

	g_main_loop_quit(loop);
}

static void dbus_name_lost(GDBusConnection *conn, const char *name, void *userdata) {
	(void)conn;
	(void)userdata;
	error("Couldn't acquire D-Bus name: %s", name);
	g_main_loop_quit(loop);
	retval = EXIT_FAILURE;
}

int main(int argc, char **argv) {

	int opt;
	const char *opts = "hVB:Si:p:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "syslog", no_argument, NULL, 'S' },
		{ "device", required_argument, NULL, 'i' },
		{ "profile", required_argument, NULL, 'p' },
		{ "a2dp-force-mono", no_argument, NULL, 6 },
		{ "a2dp-force-audio-cd", no_argument, NULL, 7 },
		{ "a2dp-keep-alive", required_argument, NULL, 8 },
		{ "a2dp-volume", no_argument, NULL, 9 },
		{ "sbc-quality", required_argument, NULL, 14 },
#if ENABLE_AAC
		{ "aac-afterburner", no_argument, NULL, 4 },
		{ "aac-latm-version", required_argument, NULL, 15 },
		{ "aac-vbr-mode", required_argument, NULL, 5 },
#endif
#if ENABLE_LDAC
		{ "ldac-abr", no_argument, NULL, 10 },
		{ "ldac-eqmid", required_argument, NULL, 11 },
#endif
#if ENABLE_MP3LAME
		{ "mp3-quality", required_argument, NULL, 12 },
		{ "mp3-vbr-quality", required_argument, NULL, 13 },
#endif
		{ 0, 0, 0, 0 },
	};

	bool syslog = false;
	char dbus_service[32] = BLUEALSA_SERVICE;

	/* Check if syslog forwarding has been enabled. This check has to be
	 * done before anything else, so we can log early stage warnings and
	 * errors. */
	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'S' /* --syslog */ :
			syslog = true;
			break;
		case 'p' /* --profile=NAME */ :
			/* reset defaults if user has specified profile option */
			memset(&config.enable, 0, sizeof(config.enable));
			break;
		}

	log_open(argv[0], syslog, BLUEALSA_LOGTIME);

	if (bluealsa_config_init() != 0) {
		error("Couldn't initialize bluealsa config");
		return EXIT_FAILURE;
	}

	/* parse options */
	optind = 0; opterr = 1;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {

		case 'h' /* --help */ :
			printf("Usage:\n"
					"  %s [OPTION]...\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -B, --dbus=NAME\tD-Bus service name suffix\n"
					"  -S, --syslog\t\tsend output to syslog\n"
					"  -i, --device=hciX\tHCI device to use\n"
					"  -p, --profile=NAME\tenable BT profile\n"
					"  --a2dp-force-mono\tforce monophonic sound\n"
					"  --a2dp-force-audio-cd\tforce 44.1 kHz sampling\n"
					"  --a2dp-keep-alive=SEC\tkeep A2DP transport alive\n"
					"  --a2dp-volume\t\tcontrol volume natively\n"
					"  --sbc-quality=NB\tset SBC quality to NB\n"
#if ENABLE_AAC
					"  --aac-afterburner\tenable afterburner\n"
					"  --aac-latm-version=NB\tset LATM syntax version\n"
					"  --aac-vbr-mode=NB\tset VBR mode to NB\n"
#endif
#if ENABLE_LDAC
					"  --ldac-abr\t\tenable adaptive bit rate\n"
					"  --ldac-eqmid=NB\tset encoder quality to NB\n"
#endif
#if ENABLE_MP3LAME
					"  --mp3-quality=NB\tset encoder quality to NB\n"
					"  --mp3-vbr-quality=NB\tset VBR quality to NB\n"
#endif
					"\nAvailable BT profiles:\n"
					"  - a2dp-source\tAdvanced Audio Source (%s)\n"
					"  - a2dp-sink\tAdvanced Audio Sink (%s)\n"
#if ENABLE_OFONO
					"  - hfp-ofono\tHands-Free handled by oFono (hf & ag)\n"
#endif
					"  - hfp-hf\tHands-Free (%s)\n"
					"  - hfp-ag\tHands-Free Audio Gateway (%s)\n"
					"  - hsp-hs\tHeadset (%s)\n"
					"  - hsp-ag\tHeadset Audio Gateway (%s)\n"
					"\n"
					"By default only output profiles are enabled, which includes A2DP Source and\n"
					"HSP/HFP Audio Gateways. If one wants to enable other set of profiles, it is\n"
					"required to explicitly specify all of them using `-p NAME` options.\n",
					argv[0],
					get_a2dp_codecs(config.a2dp.codecs, BLUEZ_A2DP_SOURCE),
					get_a2dp_codecs(config.a2dp.codecs, BLUEZ_A2DP_SINK),
					"v1.7", "v1.7", "v1.2", "v1.2");
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'B' /* --dbus=NAME */ :
			snprintf(dbus_service, sizeof(dbus_service), BLUEALSA_SERVICE ".%s", optarg);
			break;

		case 'S' /* --syslog */ :
			break;

		case 'i' /* --device=HCI */ :
			g_array_append_val(config.hci_filter, optarg);
			break;

		case 'p' /* --profile=NAME */ : {

			size_t i;
			const struct {
				char *name;
				bool *ptr;
			} map[] = {
				{ "a2dp-source", &config.enable.a2dp_source },
				{ "a2dp-sink", &config.enable.a2dp_sink },
#if ENABLE_OFONO
				{ "hfp-ofono", &config.enable.hfp_ofono },
#endif
				{ "hfp-hf", &config.enable.hfp_hf },
				{ "hfp-ag", &config.enable.hfp_ag },
				{ "hsp-hs", &config.enable.hsp_hs },
				{ "hsp-ag", &config.enable.hsp_ag },
			};

			for (i = 0; i < ARRAYSIZE(map); i++)
				if (strcasecmp(optarg, map[i].name) == 0) {
					*map[i].ptr = true;
					break;
				}

			if (i == ARRAYSIZE(map)) {
				error("Invalid BT profile name: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}

		case 6 /* --a2dp-force-mono */ :
			config.a2dp.force_mono = true;
			break;
		case 7 /* --a2dp-force-audio-cd */ :
			config.a2dp.force_44100 = true;
			break;
		case 8 /* --a2dp-keep-alive=SEC */ :
			config.a2dp.keep_alive = atoi(optarg);
			break;
		case 9 /* --a2dp-volume */ :
			config.a2dp.volume = true;
			break;

		case 14 /* --sbc-quality=NB */ :
			config.sbc_quality = atoi(optarg);
			if (config.sbc_quality > SBC_QUALITY_XQ) {
				error("Invalid encoder quality [0, %d]: %s", SBC_QUALITY_XQ, optarg);
				return EXIT_FAILURE;
			}
			if (config.sbc_quality == SBC_QUALITY_XQ) {
				info("Activating SBC Dual Channel HD (SBC XQ)");
				config.a2dp.force_44100 = true;
			}
			break;

#if ENABLE_AAC
		case 4 /* --aac-afterburner */ :
			config.aac_afterburner = true;
			break;
		case 15 /* --aac-latm-version=NB */ :
			config.aac_latm_version = atoi(optarg);
			if (config.aac_vbr_mode > 2) {
				error("Invalid LATM version [0, 2]: %s", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 5 /* --aac-vbr-mode=NB */ :
			config.aac_vbr_mode = atoi(optarg);
			if (config.aac_vbr_mode > 5) {
				error("Invalid bitrate mode [0, 5]: %s", optarg);
				return EXIT_FAILURE;
			}
			break;
#endif

#if ENABLE_LDAC
		case 10 /* --ldac-abr */ :
			config.ldac_abr = true;
			break;
		case 11 /* --ldac-eqmid=NB */ :
			config.ldac_eqmid = atoi(optarg);
			if (config.ldac_eqmid >= LDACBT_EQMID_NUM) {
				error("Invalid encoder quality index [0, %d]: %s", LDACBT_EQMID_NUM - 1, optarg);
				return EXIT_FAILURE;
			}
			break;
#endif

#if ENABLE_MP3LAME
		case 12 /* --mp3-quality=NB */ :
			config.lame_quality = atoi(optarg);
			if (config.lame_quality > 9) {
				error("Invalid encoder quality [0, 9]: %s", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 13 /* --mp3-vbr-quality=NB */ :
			config.lame_vbr_quality = atoi(optarg);
			if (config.lame_vbr_quality > 9) {
				error("Invalid VBR quality [0, 9]: %s", optarg);
				return EXIT_FAILURE;
			}
			break;
#endif

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

#if ENABLE_OFONO
	if ((config.enable.hfp_ag || config.enable.hfp_hf) && config.enable.hfp_ofono) {
		info("Disabling native HFP support due to enabled oFono profile");
		config.enable.hfp_ag = false;
		config.enable.hfp_hf = false;
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

	err = NULL;
	if (bluealsa_dbus_manager_register(&err) == 0) {
		error("Couldn't register D-Bus manager: %s", err->message);
		return EXIT_FAILURE;
	}

#if ENABLE_OFONO
	/* Enabling native HFP support while oFono is running might interfere
	 * with oFono, so in the end neither BlueALSA nor oFono will work. */
	if ((config.enable.hfp_ag || config.enable.hfp_hf) && ofono_detect_service()) {
		warn("Disabling native HFP support due to oFono service presence");
		config.enable.hfp_ag = false;
		config.enable.hfp_hf = false;
	}
#endif

	bluez_subscribe_signals();
	bluez_register();

#if ENABLE_OFONO
	ofono_subscribe_signals();
	ofono_register();
#endif

#if ENABLE_UPOWER
	upower_subscribe_signals();
	upower_initialize();
#endif

	/* In order to receive EPIPE while writing to the pipe whose reading end
	 * is closed, the SIGPIPE signal has to be handled. For more information
	 * see the io_thread_write_pcm() function. */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* register main loop exit handler */
	sigact.sa_handler = main_loop_stop;
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	/* register well-known service name */
	debug("Acquiring D-Bus service name: %s", dbus_service);
	g_bus_own_name_on_connection(config.dbus, dbus_service,
			G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, NULL, dbus_name_lost, NULL, NULL);

	/* main dispatching loop */
	debug("Starting main dispatching loop");
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);

	debug("Exiting main loop");
	return retval;
}
