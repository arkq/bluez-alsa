/*
 * BlueALSA - bluealsactl/cmd-list-services.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "bluealsactl.h"
#include "shared/dbus-client.h"

static bool print_service(const char *name, void *data) {
	(void)data;
	if (strncmp(name, BLUEALSA_SERVICE, sizeof(BLUEALSA_SERVICE) - 1) == 0)
		printf("%s\n", name);
	return true;
}

static void usage(const char *command) {
	printf("List all BlueALSA services.\n\n");
	bactl_print_usage("%s [OPTION]...", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
	);
}

static int cmd_list_services_func(int argc, char *argv[]) {

	int opt;
	const char *opts = "hqv";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "verbose", no_argument, NULL, 'v' },
		{ 0 },
	};

	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1) {
		if (bactl_parse_common_options(opt))
			continue;
		switch (opt) {
		case 'h' /* --help */ :
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			cmd_print_error("Invalid argument '%s'", argv[optind - 1]);
			return EXIT_FAILURE;
		}
	}

	if (argc != optind) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	bactl_get_ba_services(print_service, NULL, &err);
	if (dbus_error_is_set(&err)) {
		cmd_print_error("D-Bus error: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const struct bactl_command cmd_list_services = {
	"list-services",
	"List all BlueALSA services",
	cmd_list_services_func,
};
