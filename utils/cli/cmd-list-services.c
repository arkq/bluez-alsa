/*
 * BlueALSA - cmd-list-services.c
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
#include <string.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"

static bool print_service(const char *name, void *data) {
	(void) data;
	if (strncmp(name, BLUEALSA_SERVICE, sizeof(BLUEALSA_SERVICE) - 1) == 0)
		printf("%s\n", name);
	return true;
}

int cmd_list_services(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	cli_get_ba_services(print_service, NULL, &err);
	if (dbus_error_is_set(&err)) {
		cmd_print_error("D-Bus error: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
