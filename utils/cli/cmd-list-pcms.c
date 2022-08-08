/*
 * BlueALSA - cmd-list-pcms.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"

int cmd_list_pcms(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_get_pcms(&config.dbus, &pcms, &pcms_count, &err)) {
		cmd_print_error("Couldn't get BlueALSA PCM list: %s", err.message);
		return EXIT_FAILURE;
	}

	size_t i;
	for (i = 0; i < pcms_count; i++) {
		printf("%s\n", pcms[i].pcm_path);
		if (config.verbose) {
			cli_print_pcm_properties(&pcms[i], &err);
			printf("\n");
		}
	}

	free(pcms);
	return EXIT_SUCCESS;
}
