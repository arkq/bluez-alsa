/*
 * BlueALSA - cmd-delay-adjustment.c
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

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client-pcm.h"

static void usage(const char *command) {
	printf("Get or set the delay adjustment of the given PCM.\n\n");
	cli_print_usage("%s [OPTION]... PCM-PATH [ADJUSTMENT]", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
			"  ADJUSTMENT\tAdjustment value (+/-), in milliseconds\n"
	);
}

static int cmd_delay_adjustment_func(int argc, char *argv[]) {

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
		if (cli_parse_common_options(opt))
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
	if (!cli_get_ba_pcm(path, &pcm, &err)) {
		cmd_print_error("Couldn't get BlueALSA PCM: %s", err.message);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		printf("DelayAdjustment: %#.1f ms\n", (double)pcm.delay_adjustment/ 10);
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

	if (!ba_dbus_pcm_set_delay_adjustment(&config.dbus, pcm.pcm_path,
				pcm.codec.name, adjustment, &err)) {
		cmd_print_error("DelayAdjustment update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const struct cli_command cmd_delay_adjustment = {
	"delay-adjustment",
	"Get or set PCM delay adjustment",
	cmd_delay_adjustment_func,
};
