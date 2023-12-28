/*
 * BlueALSA - cmd-volume.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
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
#include "shared/dbus-client-pcm.h"

static void usage(const char *command) {
	printf("Get or set the volume value of the given PCM.\n\n");
	cli_print_usage("%s [OPTION]... PCM-PATH [VOLUME [VOLUME]]", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
			"  VOLUME\tVolume value (range depends on BT transport)\n"
	);
}

static int cmd_volume_func(int argc, char *argv[]) {

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
		cli_print_pcm_volume(&pcm);
		return EXIT_SUCCESS;
	}

	int vol1, vol2;
	vol1 = vol2 = atoi(argv[optind + 1]);
	if (argc - optind == 3)
		vol2 = atoi(argv[optind + 2]);

	if (pcm.transport & BA_PCM_TRANSPORT_MASK_A2DP) {
		if (vol1 < 0 || vol1 > 127) {
			cmd_print_error("Invalid volume [0, 127]: %d", vol1);
			return EXIT_FAILURE;
		}
		pcm.volume.ch1_volume = vol1;
		if (pcm.channels == 2) {
			if (vol2 < 0 || vol2 > 127) {
				cmd_print_error("Invalid volume [0, 127]: %d", vol2);
				return EXIT_FAILURE;
			}
			pcm.volume.ch2_volume = vol2;
		}
	}
	else {
		if (vol1 < 0 || vol1 > 15) {
			cmd_print_error("Invalid volume [0, 15]: %d", vol1);
			return EXIT_FAILURE;
		}
		pcm.volume.ch1_volume = vol1;
	}

	if (!ba_dbus_pcm_update(&config.dbus, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume loudness update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const struct cli_command cmd_volume = {
	"volume",
	"Get or set PCM audio volume",
	cmd_volume_func,
};
