/*
 * bluealsa - main.c
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

#include "bluez.h"
#include "device.h"
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
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "device", required_argument, NULL, 'i' },
		{ "disable-a2dp", no_argument, NULL, 1 },
		{ "disable-hsp", no_argument, NULL, 2 },
		{ 0, 0, 0, 0 },
	};

	struct hci_dev_info hci_dev;
	struct hci_dev_info *hci_devs;
	int hci_devs_num;
	int a2dp = 1;
	int hsp = 1;

	log_open(argv[0], 0);

	if (hci_devlist(&hci_devs, &hci_devs_num)) {
		error("Cannot enumerate devices: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	{ /* try to get default device (if possible get active one) */
		int i;
		for (i = 0; i < hci_devs_num; i++)
			if (i == 0 || hci_test_bit(HCI_UP, &hci_devs[i].flags))
				memcpy(&hci_dev, &hci_devs[i], sizeof(hci_dev));
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

		case 'i':
			warn("This feature is not implemented yet!");
			break;

		case 1:
			a2dp = 0;
			break;
		case 2:
			hsp = 0;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	/* device list is no longer required */
	free(hci_devs);

	if (!hci_devs_num) {
		error("Bluetooth adapter not available");
		return EXIT_FAILURE;
	}

	GDBusConnection *dbus;
	GHashTable *devices;
	gchar *address;
	GError *err;

	if ((devices = devices_init()) == NULL) {
		error("Cannot initialize device list structure");
		return EXIT_FAILURE;
	}

	if ((ctl_thread_init(hci_dev.name, devices)) == -1) {
		error("Cannot initialize controller thread: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	err = NULL;
	address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if ((dbus = g_dbus_connection_new_for_address_sync(address,
					G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
					G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
					NULL, NULL, &err)) == NULL) {
		error("Cannot obtain D-Bus connection: %s", err->message);
		return EXIT_FAILURE;
	}

	if (a2dp)
		bluez_register_a2dp(dbus, hci_dev.name, devices);
	if (hsp)
		bluez_register_hsp(dbus, hci_dev.name, devices);

	bluez_subscribe_signals(dbus, hci_dev.name, devices);

	struct sigaction sigact = { .sa_handler = main_loop_stop };
	sigaction(SIGINT, &sigact, NULL);

	/* main dispatching loop */
	debug("Starting main dispatching loop");
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);

	debug("Exiting main loop");

	ctl_free();
	return EXIT_SUCCESS;
}
