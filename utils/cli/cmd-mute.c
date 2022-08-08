/*
 * BlueALSA - cmd-mute.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"

int cmd_mute(int argc, char *argv[]) {

	if (argc < 2 || argc > 4) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!cli_get_ba_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		cli_print_pcm_mute(&pcm);
		return EXIT_SUCCESS;
	}

	pcm.volume.ch1_muted = pcm.volume.ch2_muted = false;

	if (strcasecmp(argv[2], "y") == 0)
		pcm.volume.ch1_muted = pcm.volume.ch2_muted = true;
	else if (strcasecmp(argv[2], "n") != 0) {
		cmd_print_error("Invalid argument [y|n]: %s", argv[2]);
		return EXIT_FAILURE;
	}

	if (pcm.channels == 2 && argc == 4) {
		if (strcasecmp(argv[3], "y") == 0)
			pcm.volume.ch2_muted = true;
		else if (strcasecmp(argv[3], "n") != 0) {
			cmd_print_error("Invalid argument [y|n]: %s", argv[3]);
			return EXIT_FAILURE;
		}
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&config.dbus, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume mute update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
