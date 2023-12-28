/*
 * BlueALSA - cmd-mute.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
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

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client-pcm.h"

static void usage(const char *command) {
	printf("Get or set the mute switch of the given PCM.\n\n");
	cli_print_usage("%s [OPTION]... PCM-PATH [STATE [STATE]]", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
			"  STATE\t\tEnable or disable mute switch\n"
	);
}

static int cmd_mute_func(int argc, char *argv[]) {

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

	if (argc - optind < 1) {
		cmd_print_error("Missing BlueALSA PCM path argument");
		return EXIT_FAILURE;
	}
	if (argc - optind > 3) {
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

	if (argc - optind == 1) {
		cli_print_pcm_mute(&pcm);
		return EXIT_SUCCESS;
	}

	const char *value;
	bool state;

	value = argv[optind + 1];
	if (!cli_parse_value_on_off(value, &state)) {
		cmd_print_error("Invalid argument: %s", value);
		return EXIT_FAILURE;
	}

	pcm.volume.ch1_muted = state;
	pcm.volume.ch2_muted = state;

	if (pcm.channels == 2 && argc - optind == 3) {

		value = argv[optind + 2];
		if (!cli_parse_value_on_off(value, &state)) {
			cmd_print_error("Invalid argument: %s", value);
			return EXIT_FAILURE;
		}

		pcm.volume.ch2_muted = state;

	}

	if (!ba_dbus_pcm_update(&config.dbus, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume mute update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const struct cli_command cmd_mute = {
	"mute",
	"Get or set PCM mute switch",
	cmd_mute_func,
};
