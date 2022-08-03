/*
 * BlueALSA - cmd-status.c
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

int cmd_status(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	struct ba_service_props props = { 0 };

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_get_props(&config.dbus, &props, &err)) {
		cmd_print_error("D-Bus error: %s", err.message);
		bluealsa_dbus_props_free(&props);
		return EXIT_FAILURE;
	}

	printf("Service: %s\n", config.dbus.ba_service);
	printf("Version: %s\n", props.version);
	cli_print_adapters(&props);
	cli_print_profiles_and_codecs(&props);

	bluealsa_dbus_props_free(&props);
	return EXIT_SUCCESS;
}
