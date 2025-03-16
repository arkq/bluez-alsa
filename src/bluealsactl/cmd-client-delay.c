/*
 * BlueALSA - bluealsactl/cmd-client-delay.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "bluealsactl.h"
#include "shared/dbus-client-pcm.h"

static void usage(const char *command) {
	printf("Get or set the client delay of the given PCM.\n\n");
	bactl_print_usage("%s [OPTION]... PCM-PATH [[-]DELAY]", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
			"  DELAY\tValue (+/-), in milliseconds\n"
	);
}

static int cmd_client_delay_func(int argc, char *argv[]) {

	int opt;
	const char *opts = "+hqv";
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
		if (opt == 'h') { /* --help */
			usage(argv[0]);
			return EXIT_SUCCESS;
		}
	}

	if (argc - optind < 1) {
		cmd_print_error("Missing BlueALSA PCM path argument");
		return EXIT_FAILURE;
	}
	if (argc - optind > 2) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	const char *path = argv[optind];

	struct ba_pcm pcm;
	if (!bactl_get_ba_pcm(path, &pcm, &err)) {
		cmd_print_error("Couldn't get BlueALSA PCM: %s", err.message);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		bactl_print_pcm_client_delay(&pcm);
		return EXIT_SUCCESS;
	}

	const char *value = argv[optind + 1];
	errno = 0;
	char *endptr = NULL;
	double dbl = strtod(value, &endptr);
	if (endptr == value || errno == ERANGE) {
		cmd_print_error("Invalid argument: %s", value);
		return EXIT_FAILURE;
	}

	int adjustment = lround(dbl * 10.0);
	if (adjustment < INT16_MIN || adjustment > INT16_MAX) {
		cmd_print_error("Invalid argument: %s", value);
		return EXIT_FAILURE;
	}

	pcm.client_delay = adjustment;
	if (!ba_dbus_pcm_update(&config.dbus, &pcm, BLUEALSA_PCM_CLIENT_DELAY, &err)) {
		cmd_print_error("ClientDelay update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const struct bactl_command cmd_client_delay = {
	"client-delay",
	"Get or set PCM client delay",
	cmd_client_delay_func,
};
