/*
 * BlueALSA - bluealsactl/cmd-volume.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "bluealsactl.h"
#include "shared/dbus-client-pcm.h"

static void usage(const char *command) {
	printf("Get or set the volume value of the given PCM.\n\n");
	bactl_print_usage("%s [OPTION]... PCM-PATH [VOLUME [VOLUME]...]", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
			"  VOLUME\tVolume value (range depends on BT transport)\n"
	);
}

static int cmd_volume_func(int argc, char *argv[]) {

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
		bactl_print_pcm_volume(&pcm);
		return EXIT_SUCCESS;
	}

	if (argc - optind - 1 > pcm.channels) {
		cmd_print_error("Invalid number of channels: %d > %d",
				argc - optind - 1, pcm.channels);
		return EXIT_FAILURE;
	}

	const int v_min = 0;
	const int v_max = pcm.transport & BA_PCM_TRANSPORT_MASK_A2DP ? 127 : 15;

	for (size_t i = 0; i < (size_t)argc - optind - 1; i++) {

		const int v = atoi(argv[optind + 1 + i]);
		pcm.volume[i].volume = v;

		if (v < v_min || v > v_max) {
			cmd_print_error("Invalid volume [%d, %d]: %d", v_min, v_max, v);
			return EXIT_FAILURE;
		}

	}

	/* Upscale volume values to update all PCM channels. */
	for (size_t i = argc - optind - 1; i < pcm.channels; i++)
		pcm.volume[i].volume = pcm.volume[0].volume;

	if (!ba_dbus_pcm_update(&config.dbus, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const struct bactl_command cmd_volume = {
	"volume",
	"Get or set PCM audio volume",
	cmd_volume_func,
};
