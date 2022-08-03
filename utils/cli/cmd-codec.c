/*
 * BlueALSA - cmd-codec.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"
#include "shared/hex.h"

int cmd_codec(int argc, char *argv[]) {

	if (argc < 2 || argc > 4) {
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

	if (argc == 2) {
		cli_print_pcm_codecs(path, &err);
		printf("Selected codec: %s\n", pcm.codec.name);
		return EXIT_SUCCESS;
	}

	const char *codec = argv[2];
	int result = EXIT_FAILURE;

	uint8_t codec_config[64];
	ssize_t codec_config_len = 0;

	if (argc == 4) {
		size_t codec_config_hex_len;
		if ((codec_config_hex_len = strlen(argv[3])) > sizeof(codec_config) * 2) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "Invalid codec configuration: %s", argv[3]);
			goto fail;
		}
		if ((codec_config_len = hex2bin(argv[3], codec_config, codec_config_hex_len)) == -1) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "%s", strerror(errno));
			goto fail;
		}
	}

	if (!bluealsa_dbus_pcm_select_codec(&config.dbus, path,
				bluealsa_dbus_pcm_get_codec_canonical_name(codec), codec_config, codec_config_len, &err))
		goto fail;

	result = EXIT_SUCCESS;

fail:
	if (dbus_error_is_set(&err))
		cmd_print_error("Couldn't select BlueALSA PCM Codec: %s", err.message);
	return result;
}
