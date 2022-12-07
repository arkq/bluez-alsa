/*
 * BlueALSA - cmd-open.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"

static void usage(const char *command) {
	printf("Transfer raw PCM data via stdin or stdout.\n\n");
	cli_print_usage("%s [OPTION]... PCM-PATH", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
	);
}

static int cmd_open_func(int argc, char *argv[]) {

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
	if (argc - optind > 2) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[optind];
	if (!dbus_validate_path(path, NULL)) {
		cmd_print_error("Invalid PCM path: %s", path);
		return EXIT_FAILURE;
	}

	int fd_pcm, fd_pcm_ctrl, input, output;
	size_t len = strlen(path);

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_open(&config.dbus, path, &fd_pcm, &fd_pcm_ctrl, &err)) {
		cmd_print_error("Cannot open PCM: %s", err.message);
		return EXIT_FAILURE;
	}

	if (strcmp(path + len - strlen("source"), "source") == 0) {
		input = fd_pcm;
		output = STDOUT_FILENO;
	}
	else {
		input = STDIN_FILENO;
		output = fd_pcm;
	}

	ssize_t count;
	char buffer[4096];
	while ((count = read(input, buffer, sizeof(buffer))) > 0) {
		ssize_t written = 0;
		const char *pos = buffer;
		while (written < count) {
			ssize_t res = write(output, pos, count - written);
			if (res <= 0) {
				/* Cannot write any more, so just terminate */
				goto finish;
			}
			written += res;
			pos += res;
		}
	}

	if (output == fd_pcm)
		bluealsa_dbus_pcm_ctrl_send_drain(fd_pcm_ctrl, &err);

finish:
	close(fd_pcm);
	close(fd_pcm_ctrl);
	return EXIT_SUCCESS;
}

const struct cli_command cmd_open = {
	"open",
	"Transfer raw PCM via stdin or stdout",
	cmd_open_func,
};
