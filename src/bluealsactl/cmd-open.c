/*
 * BlueALSA - bluealsactl/cmd-open.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "bluealsactl.h"
#include "shared/dbus-client-pcm.h"
#include "shared/hex.h"

static void usage(const char *command) {
	printf("Transfer raw PCM data via stdin or stdout.\n\n");
	bactl_print_usage("%s [OPTION]... PCM-PATH", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"  -x, --hex\t\tTransfer data in hexadecimal format\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
	);
}

static int cmd_open_func(int argc, char *argv[]) {

	int opt;
	const char *opts = "hqvx";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "hex", no_argument, NULL, 'x' },
		{ 0 },
	};

	bool hex = false;

	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1) {
		if (bactl_parse_common_options(opt))
			continue;
		switch (opt) {
		case 'h' /* --help */ :
			usage(argv[0]);
			return EXIT_SUCCESS;
		case 'x' /* --hex */ :
			hex = true;
			break;
		default:
			cmd_print_error("Invalid argument '%s'", argv[optind - 1]);
			return EXIT_FAILURE;
		}
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

	int fd_pcm, fd_pcm_ctrl;
	int fd_input, fd_output;
	size_t len = strlen(path);

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_pcm_open(&config.dbus, path, &fd_pcm, &fd_pcm_ctrl, &err)) {
		cmd_print_error("Couldn't open PCM: %s", err.message);
		return EXIT_FAILURE;
	}

	if (strcmp(path + len - strlen("source"), "source") == 0) {
		fd_input = fd_pcm;
		fd_output = STDOUT_FILENO;
	}
	else {
		fd_input = STDIN_FILENO;
		fd_output = fd_pcm;
	}

	uint8_t buffer[4096];
	uint8_t buffer_bin[sizeof(buffer) / 2];
	char buffer_hex[sizeof(buffer) * 2 + 1];
	ssize_t count;

	while ((count = read(fd_input, buffer, sizeof(buffer))) > 0) {

		const uint8_t *pos = buffer;
		ssize_t written = 0;

		if (hex) {

			if (fd_input == STDIN_FILENO) {
				pos = buffer_bin;
				if ((count = hex2bin((const char *)buffer, buffer_bin, count)) == -1) {
					cmd_print_error("Couldn't decode hex string: %s", strerror(errno));
					continue;
				}
			}

			if (fd_output == STDOUT_FILENO) {
				pos = (const uint8_t *)buffer_hex;
				count = bin2hex(buffer, buffer_hex, count);
			}

		}

		while (written < count) {
			ssize_t res = write(fd_output, pos, count - written);
			if (res <= 0) {
				/* Cannot write any more, so just terminate */
				goto finish;
			}
			written += res;
			pos += res;
		}

	}

	if (fd_output == fd_pcm)
		ba_dbus_pcm_ctrl_send_drain(fd_pcm_ctrl, &err);

finish:
	close(fd_pcm);
	close(fd_pcm_ctrl);
	return EXIT_SUCCESS;
}

const struct bactl_command cmd_open = {
	"open",
	"Transfer raw PCM via stdin or stdout",
	cmd_open_func,
};
