/*
 * BlueALSA - main.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <glib.h>
#include <gio/gio.h>

#include "bluealsa.h"
#include "bluez.h"
#include "ctl.h"
#include "transport.h"
#include "utils.h"
#include "shared/log.h"


static GMainLoop *loop = NULL;
static void main_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);

	g_main_loop_quit(loop);
}

int main(int argc, char **argv) {

	int opt;
	const char *opts = "hVSi:p:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "syslog", no_argument, NULL, 'S' },
		{ "device", required_argument, NULL, 'i' },
		{ "profile", required_argument, NULL, 'p' },
#if ENABLE_AAC
		{ "aac-afterburner", no_argument, NULL, 4 },
		{ "aac-vbr-mode", required_argument, NULL, 5 },
#endif
		{ "a2dp-force-mono", no_argument, NULL, 6 },
		{ "a2dp-force-audio-cd", no_argument, NULL, 7 },
		{ "a2dp-volume", no_argument, NULL, 8 },
		{ 0, 0, 0, 0 },
	};

	bool syslog = false;
	struct hci_dev_info *hci_devs;
	int hci_devs_num;

	/* Check if syslog forwarding has been enabled. This check has to be
	 * done before anything else, so we can log early stage warnings and
	 * errors. */
	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'S':
			syslog = true;
			break;
		case 'p':
			/* reset defaults if user has specified profile option */
			memset(&config.enable, 0, sizeof(config.enable));
			break;
		}

	log_open(argv[0], syslog, BLUEALSA_LOGTIME);

	if (bluealsa_config_init() != 0) {
		error("Couldn't initialize bluealsa config");
		return EXIT_FAILURE;
	}

	if (hci_devlist(&hci_devs, &hci_devs_num)) {
		error("Couldn't enumerate HCI devices: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	if (!hci_devs_num) {
		error("No HCI device available");
		return EXIT_FAILURE;
	}

	{ /* try to get default device (if possible get active one) */
		int i;
		for (i = 0; i < hci_devs_num; i++)
			if (i == 0 || hci_test_bit(HCI_UP, &hci_devs[i].flags))
				memcpy(&config.hci_dev, &hci_devs[i], sizeof(config.hci_dev));
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
					"  -S, --syslog\t\tsend output to syslog\n"
					"  -i, --device=hciX\tHCI device to use\n"
					"  -p, --profile=NAME\tenable BT profile\n"
					"  --a2dp-force-mono\tforce monophonic sound\n"
					"  --a2dp-force-audio-cd\tforce 44.1 kHz sampling\n"
					"  --a2dp-volume\t\tcontrol volume natively\n"
#if ENABLE_AAC
					"  --aac-afterburner\tenable afterburner\n"
					"  --aac-vbr-mode=NB\tset VBR mode to NB\n"
#endif
					"\nAvailable BT profiles:\n"
					"  - a2dp-source\tAdvanced Audio Source\n"
					"  - a2dp-sink\tAdvanced Audio Sink\n"
					"  - hsp-hs\tHeadset\n"
					"  - hsp-ag\tHeadset Audio Gateway\n"
					"  - hfp-hf\tHands-Free\n"
					"  - hfp-ag\tHands-Free Audio Gateway\n"
					"\n"
					"By default only output profiles are enabled, which includes A2DP Source and\n"
					"HSP/HFP Audio Gateways. If one wants to enable other set of profiles, it is\n"
					"required to explicitly specify all of them using `-p NAME` options.\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'S' /* --syslog */ :
			break;

		case 'i' /* --device=HCI */ : {

			bdaddr_t addr;
			int i = hci_devs_num;
			int found = 0;

			if (str2ba(optarg, &addr) == 0) {
				while (i--)
					if (bacmp(&addr, &hci_devs[i].bdaddr) == 0) {
						memcpy(&config.hci_dev, &hci_devs[i], sizeof(config.hci_dev));
						found = 1;
						break;
					}
			}
			else {
				while (i--)
					if (strcmp(optarg, hci_devs[i].name) == 0) {
						memcpy(&config.hci_dev, &hci_devs[i], sizeof(config.hci_dev));
						found = 1;
						break;
				}
			}

			if (found == 0) {
				error("HCI device not found: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}

		case 'p' /* --profile=NAME */ : {

			size_t i;
			const struct {
				char *name;
				bool *ptr;
			} map[] = {
				{ "a2dp-source", &config.enable.a2dp_source },
				{ "a2dp-sink", &config.enable.a2dp_sink },
				{ "hsp-hs", &config.enable.hsp_hs },
				{ "hsp-ag", &config.enable.hsp_ag },
				{ "hfp-hf", &config.enable.hfp_hf },
				{ "hfp-ag", &config.enable.hfp_ag },
			};

			for (i = 0; i < sizeof(map) / sizeof(*map); i++)
				if (strcasecmp(optarg, map[i].name) == 0) {
					*map[i].ptr = true;
					break;
				}

			if (i == sizeof(map) / sizeof(*map)) {
				error("Invalid BT profile name: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}

#if ENABLE_AAC
		case 4 /* --aac-afterburner */ :
			config.aac_afterburner = true;
			break;
		case 5 /* --aac-vbr-mode=NB */ :
			config.aac_vbr_mode = atoi(optarg);
			if (config.aac_vbr_mode > 5) {
				error("Invalid bitrate mode [0, 5]: %s", optarg);
				return EXIT_FAILURE;
			}
			break;
#endif

		case 6 /* --a2dp-force-mono */ :
			config.a2dp_force_mono = true;
			break;
		case 7 /* --a2dp-force-audio-cd */ :
			config.a2dp_force_44100 = true;
			break;
		case 8 /* --a2dp-volume */ :
			config.a2dp_volume = true;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	/* device list is no longer required */
	free(hci_devs);

	/* initialize random number generator */
	srandom(time(NULL));

	if ((bluealsa_ctl_thread_init()) == -1) {
		error("Couldn't initialize controller thread: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	gchar *address;
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

	bluez_subscribe_signals();

	bluez_register_a2dp();
	bluez_register_hfp();

	/* In order to receive EPIPE while writing to the pipe whose reading end
	 * is closed, the SIGPIPE signal has to be handled. For more information
	 * see the io_thread_write_pcm() function. */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* register main loop exit handler */
	sigact.sa_handler = main_loop_stop;
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	/* main dispatching loop */
	debug("Starting main dispatching loop");
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);

	debug("Exiting main loop");

	/* From all of the cleanup routines, these ones cannot be omitted. We have
	 * to unlink named sockets, otherwise service will not start any more. */
	bluealsa_ctl_free();
	bluealsa_config_free();

	return EXIT_SUCCESS;
}
