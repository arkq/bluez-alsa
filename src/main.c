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
#include <dbus/dbus.h>
#include <glib.h>

#include "bluez.h"
#include "device.h"
#include "ctl.h"
#include "log.h"
#include "transport.h"
#include "utils.h"


static DBusConnection *dbus = NULL;
static void main_loop_stop(int sig) {
	(void)(sig);

	/* NOTE: Call to this handler restores the default action, so on the
	 *       second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(SIGINT, &sigact, NULL);

	dbus_connection_close(dbus);
}

int main(int argc, char **argv) {

	int opt;
	const char *opts = "hi:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "device", required_argument, NULL, 'i' },
		{ 0, 0, 0, 0 },
	};

	struct hci_dev_info hci_dev;
	struct hci_dev_info *hci_devs;
	int hci_devs_num;

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
					"  -i, --device=hciX\tHCI device to use\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'i':
			warn("This feature is not implemented yet!");
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

	GHashTable *devices;
	DBusError err;

	if ((devices = devices_init()) == NULL) {
		error("Cannot initialize device list structure");
		return EXIT_FAILURE;
	}

	if (transport_threads_init() == -1) {
		error("Cannot initialize transport threads: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	if (!dbus_threads_init_default()) {
		error("Cannot initialize D-Bus threads");
		return EXIT_FAILURE;
	}

	dbus_error_init(&err);
	if ((dbus = dbus_bus_get(DBUS_BUS_SYSTEM, &err)) == NULL) {
		error("Cannot obtain D-Bus connection: %s", err.message);
		return EXIT_FAILURE;
	}

	if ((ctl_thread_init(hci_dev.name, devices)) == -1) {
		error("Cannot initialize controller thread: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	bluez_register_a2dp(dbus, hci_dev.name, devices);
	bluez_register_hsp(dbus, devices);
	bluez_register_signal_handler(dbus, hci_dev.name, devices);

	struct sigaction sigact = { .sa_handler = main_loop_stop };
	sigaction(SIGINT, &sigact, NULL);

	/* main dispatching loop */
	debug("Starting main dispatching loop");
	dbus_connection_set_exit_on_disconnect(dbus, FALSE);
	while (dbus_connection_read_write_dispatch(dbus, -1))
		continue;

	debug("Exiting main loop");

	ctl_free();
	dbus_connection_unref(dbus);
	return EXIT_SUCCESS;
}
