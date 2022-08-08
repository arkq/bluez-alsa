/*
 * BlueALSA - cmd-info.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdlib.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"
#include "shared/log.h"

int cmd_info(int argc, char *argv[]) {

	if (argc != 2) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!cli_get_ba_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	cli_print_pcm_properties(&pcm, &err);
	if (dbus_error_is_set(&err))
		warn("Unable to read available codecs: %s", err.message);

	return EXIT_SUCCESS;
}
