/*
 * BlueALSA - cmd-codec.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdbool.h>
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
	cli_print_usage("%s [OPTION]... PCM-PATH [CODEC[:CONFIG]]", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
			"  -f, --force\t\tForce codec configuration (skip conformance check)\n"
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
	const char *opts = "hqvf";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "force", no_argument, NULL, 'f' },
		{ 0 },
	};

	bool force = false;

	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1) {
		if (cli_parse_common_options(opt))
			continue;
		switch (opt) {
		case 'h' /* --help */ :
			usage(argv[0]);
			return EXIT_SUCCESS;
		case 'f' /* --force */ :
			force = true;
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

	char *codec = argv[optind + 1];
	uint8_t codec_config[64] = { 0 };
	ssize_t codec_config_len = 0;
	unsigned int flags = BA_PCM_SELECT_CODEC_FLAG_NONE;
	int result = EXIT_FAILURE;

	char *codec_config_hex;
	/* split the given string into name and configuration components */
	if ((codec_config_hex = strchr(codec, ':')) != NULL) {
		*codec_config_hex++ = '\0';

		size_t codec_config_hex_len;
		if ((codec_config_hex_len = strlen(codec_config_hex)) > sizeof(codec_config) * 2) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "Invalid codec configuration: %s", codec_config_hex);
			goto fail;
		}

		if ((codec_config_len = hex2bin(codec_config_hex, codec_config, codec_config_hex_len)) == -1) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "%s", strerror(errno));
			goto fail;
		}

	}

	if (force)
		flags |= BA_PCM_SELECT_CODEC_FLAG_NON_CONFORMANT;

	if (!ba_dbus_pcm_select_codec(&config.dbus, path,
				ba_dbus_pcm_codec_get_canonical_name(codec),
				codec_config, codec_config_len, flags, &err))
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
