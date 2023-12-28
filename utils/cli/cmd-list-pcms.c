/*
 * BlueALSA - cmd-list-pcms.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
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
	printf("List all BlueALSA PCM paths.\n\n");
	cli_print_usage("%s [OPTION]...", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
	);
}

static int cmd_list_pcms_func(int argc, char *argv[]) {

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

	if (argc != optind) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_pcm_get_all(&config.dbus, &pcms, &pcms_count, &err)) {
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

const struct cli_command cmd_list_pcms = {
	"list-pcms",
	"List all BlueALSA PCM paths",
	cmd_list_pcms_func,
};
