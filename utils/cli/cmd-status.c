/*
 * BlueALSA - cmd-status.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"

static void usage(const char *command) {
	printf("Show BlueALSA service runtime status.\n\n");
	cli_print_usage("%s [OPTION]...", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
	);
}

static int cmd_status_func(int argc, char *argv[]) {

	int opt;
	const char *opts = "h";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};

	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			cmd_print_error("Invalid argument '%s'", argv[optind - 1]);
			return EXIT_FAILURE;
		}

	if (argc != optind) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	struct ba_service_props props = { 0 };

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_service_props_get(&config.dbus, &props, &err)) {
		cmd_print_error("D-Bus error: %s", err.message);
		ba_dbus_service_props_free(&props);
		return EXIT_FAILURE;
	}

	printf("Service: %s\n", config.dbus.ba_service);
	printf("Version: %s\n", props.version);
	cli_print_adapters(&props);
	cli_print_profiles_and_codecs(&props);

	ba_dbus_service_props_free(&props);
	return EXIT_SUCCESS;
}

const struct cli_command cmd_status = {
	"status",
	"Show BlueALSA service status",
	cmd_status_func,
};
