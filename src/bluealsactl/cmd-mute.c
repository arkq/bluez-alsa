/*
 * BlueALSA - bluealsactl/cmd-mute.c
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
#include <unistd.h>

#include <dbus/dbus.h>

#include "bluealsactl.h"
#include "shared/dbus-client-pcm.h"

static void usage(const char *command) {
	printf("Get or set the mute switch of the given PCM.\n\n");
	bactl_print_usage("%s [OPTION]... PCM-PATH [STATE [STATE]...]", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
			"  STATE\t\tEnable or disable mute switch\n"
	);
}

static int cmd_mute_func(int argc, char *argv[]) {

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

	if (argc - optind < 1) {
		cmd_print_error("Missing BlueALSA PCM path argument");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	const char *path = argv[optind];

	struct ba_pcm pcm;
	if (!bactl_get_ba_pcm(path, &pcm, &err)) {
		cmd_print_error("Couldn't get BlueALSA PCM: %s", err.message);
		return EXIT_FAILURE;
	}

	if (argc - optind == 1) {
		bactl_print_pcm_mute(&pcm);
		return EXIT_SUCCESS;
	}

	if (argc - optind - 1 > pcm.channels) {
		cmd_print_error("Invalid number of channels: %d > %d",
				argc - optind - 1, pcm.channels);
		return EXIT_FAILURE;
	}

	for (size_t i = 0; i < (size_t)argc - optind - 1; i++) {

		bool state;
		if (!bactl_parse_value_on_off(argv[optind + 1 + i], &state)) {
			cmd_print_error("Invalid mute value: %s", argv[optind + 1 + i]);
			return EXIT_FAILURE;
		}

		pcm.volume[i].muted = state;

	}

	/* Upscale mute switch values to update all PCM channels. */
	for (size_t i = argc - optind - 1; i < pcm.channels; i++)
		pcm.volume[i].muted = pcm.volume[0].muted;

	if (!ba_dbus_pcm_update(&config.dbus, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume mute update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const struct bactl_command cmd_mute = {
	"mute",
	"Get or set PCM mute switch",
	cmd_mute_func,
};
