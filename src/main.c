/*
 * BlueALSA - main.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

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
#include "log.h"
#include "transport.h"
#include "utils.h"


static GMainLoop *loop = NULL;
static void main_loop_stop(int sig) {
	(void)(sig);

	/* NOTE: Call to this handler restores the default action, so on the
	 *       second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(SIGINT, &sigact, NULL);

	g_main_loop_quit(loop);
}

int main(int argc, char **argv) {

	int opt;
	const char *opts = "hi:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "device", required_argument, NULL, 'i' },
		{ "disable-a2dp", no_argument, NULL, 1 },
		{ "disable-hsp", no_argument, NULL, 2 },
		{ 0, 0, 0, 0 },
	};

	struct ba_setup setup;
	struct hci_dev_info *hci_devs;
	int hci_devs_num;

	log_open(argv[0], 0);

	if (bluealsa_setup_init(&setup) != 0) {
		error("Couldn't initialize bluealsa setup");
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
				memcpy(&setup.hci_dev, &hci_devs[i], sizeof(setup.hci_dev));
	}

	/* parse options */
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("usage: %s [ -hi ]\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -i, --device=hciX\tHCI device to use\n"
					"  --disable-a2dp\tdisable A2DP support\n"
					"  --disable-hsp\t\tdisable HSP support\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'i': {

			bdaddr_t addr;
			int i = hci_devs_num;
			int found = 0;

			if (str2ba(optarg, &addr) == 0) {
				while (i--)
					if (bacmp(&addr, &hci_devs[i].bdaddr) == 0) {
						memcpy(&setup.hci_dev, &hci_devs[i], sizeof(setup.hci_dev));
						found = 1;
						break;
					}
			}
			else {
				while (i--)
					if (strcmp(optarg, hci_devs[i].name) == 0) {
						memcpy(&setup.hci_dev, &hci_devs[i], sizeof(setup.hci_dev));
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

		case 1:
			setup.enable_a2dp = 0;
			break;
		case 2:
			setup.enable_hsp = 0;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	/* device list is no longer required */
	free(hci_devs);

	/* initialize random generator */
	srandom(time(NULL));

	if ((bluealsa_ctl_thread_init(&setup)) == -1) {
		error("Couldn't initialize controller thread: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	GDBusConnection *dbus;
	gchar *address;
	GError *err;

	err = NULL;
	address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if ((dbus = g_dbus_connection_new_for_address_sync(address,
					G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
					G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
					NULL, NULL, &err)) == NULL) {
		error("Couldn't obtain D-Bus connection: %s", err->message);
		return EXIT_FAILURE;
	}

	bluez_subscribe_signals(dbus, &setup);

	if (setup.enable_a2dp)
		bluez_register_a2dp(dbus, &setup);
	if (setup.enable_hsp)
		bluez_register_hsp(dbus, &setup);

	struct sigaction sigact = { .sa_handler = main_loop_stop };
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	/* main dispatching loop */
	debug("Starting main dispatching loop");
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);

	debug("Exiting main loop");

	/* From all of the cleanup routines, these ones cannot be omitted. We have
	 * to unlink named sockets, otherwise service will not start any more. */
	bluealsa_ctl_free(&setup);
	bluealsa_setup_free(&setup);

	return EXIT_SUCCESS;
}
