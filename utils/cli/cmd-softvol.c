/*
 * BlueALSA - cmd-softvol.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"

int cmd_softvol(int argc, char *argv[]) {

	if (argc < 2 || argc > 3) {
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
		printf("SoftVolume: %c\n", pcm.soft_volume ? 'Y' : 'N');
		return EXIT_SUCCESS;
	}

	if (strcasecmp(argv[2], "y") == 0)
		pcm.soft_volume = true;
	else if (strcasecmp(argv[2], "n") == 0)
		pcm.soft_volume = false;
	else {
		cmd_print_error("Invalid argument [y|n]: %s", argv[2]);
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&config.dbus, &pcm, BLUEALSA_PCM_SOFT_VOLUME, &err)) {
		cmd_print_error("SoftVolume update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
