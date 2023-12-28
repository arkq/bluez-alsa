/*
 * BlueALSA - cmd-codec.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client-pcm.h"
#include "shared/hex.h"

static void usage(const char *command) {
	printf("Get or set the Bluetooth codec used by the given PCM.\n\n");
	cli_print_usage("%s [OPTION]... PCM-PATH [CODEC [CONFIG]]", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"\nPositional arguments:\n"
			"  PCM-PATH\tBlueALSA PCM D-Bus object path\n"
			"  CODEC\t\tCodec identifier for setting new codec\n"
			"  CONFIG\tOptional configuration for new codec\n"
			"\nNote:\n"
			"  This command requires BlueZ version >= 5.52 for SEP support.\n"
	);
}

static int cmd_codec_func(int argc, char *argv[]) {

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
	if (argc - optind > 3) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	const char *path = argv[optind];

	struct ba_pcm pcm;
	if (!cli_get_ba_pcm(path, &pcm, &err)) {
		cmd_print_error("Couldn't get BlueALSA PCM: %s", err.message);
		return EXIT_FAILURE;
	}

	if (argc - optind == 1) {
		cli_print_pcm_available_codecs(&pcm, NULL);
		cli_print_pcm_selected_codec(&pcm);
		return EXIT_SUCCESS;
	}

	const char *codec = argv[optind + 1];
	int result = EXIT_FAILURE;

	uint8_t codec_config[64];
	ssize_t codec_config_len = 0;

	if (argc - optind == 3) {
		size_t codec_config_hex_len;
		const char *codec_config_hex = argv[optind + 2];
		if ((codec_config_hex_len = strlen(codec_config_hex)) > sizeof(codec_config) * 2) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "Invalid codec configuration: %s", codec_config_hex);
			goto fail;
		}
		if ((codec_config_len = hex2bin(codec_config_hex, codec_config, codec_config_hex_len)) == -1) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "%s", strerror(errno));
			goto fail;
		}
	}

	if (!ba_dbus_pcm_select_codec(&config.dbus, path,
				ba_dbus_pcm_codec_get_canonical_name(codec), codec_config, codec_config_len, &err))
		goto fail;

	result = EXIT_SUCCESS;

fail:
	if (dbus_error_is_set(&err))
		cmd_print_error("Couldn't select BlueALSA PCM Codec: %s", err.message);
	return result;
}

const struct cli_command cmd_codec = {
	"codec",
	"Get or set PCM Bluetooth codec",
	cmd_codec_func,
};
