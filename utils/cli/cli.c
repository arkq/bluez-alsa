/*
 * BlueALSA - cli.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"
#include "shared/hex.h"
#include "shared/log.h"

/**
 * Helper macros for internal usage. */
#define cli_print_error(M, ...) if (!quiet) { error(M, ##__VA_ARGS__); }
#define cmd_print_error(M, ...) if (!quiet) { error("CMD \"%s\": " M, argv[0], ##__VA_ARGS__); }

static struct ba_dbus_ctx dbus_ctx;
static char dbus_ba_service[32] = BLUEALSA_SERVICE;
static bool quiet = false;

static const char *transport_code_to_string(int transport_code) {
	switch (transport_code) {
	case BA_PCM_TRANSPORT_A2DP_SOURCE:
		return "A2DP-source";
	case BA_PCM_TRANSPORT_A2DP_SINK:
		return"A2DP-sink";
	case BA_PCM_TRANSPORT_HFP_AG:
		return "HFP-AG";
	case BA_PCM_TRANSPORT_HFP_HF:
		return "HFP-HF";
	case BA_PCM_TRANSPORT_HSP_AG:
		return "HSP-AG";
	case BA_PCM_TRANSPORT_HSP_HS:
		return "HSP-HS";
	case BA_PCM_TRANSPORT_MASK_A2DP:
		return "A2DP";
	case BA_PCM_TRANSPORT_MASK_HFP:
		return "HFP";
	case BA_PCM_TRANSPORT_MASK_HSP:
		return "HSP";
	case BA_PCM_TRANSPORT_MASK_SCO:
		return "SCO";
	case BA_PCM_TRANSPORT_MASK_AG:
		return "AG";
	case BA_PCM_TRANSPORT_MASK_HF:
		return "HF";
	default:
		return "Invalid";
	}
}

static const char *pcm_mode_to_string(int pcm_mode) {
	switch (pcm_mode) {
	case BA_PCM_MODE_SINK:
		return "sink";
	case BA_PCM_MODE_SOURCE:
		return "source";
	default:
		return "Invalid";
	}
}

static const char *pcm_format_to_string(int pcm_format) {
	switch (pcm_format) {
	case 0x0108:
		return "U8";
	case 0x8210:
		return "S16_LE";
	case 0x8318:
		return "S24_3LE";
	case 0x8418:
		return "S24_LE";
	case 0x8420:
		return "S32_LE";
	default:
		return "Invalid";
	}
}

static bool get_pcm(const char *path, struct ba_pcm *pcm) {

	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;
	bool found = false;
	size_t i;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &pcms, &pcms_count, &err))
		return false;

	for (i = 0; i < pcms_count; i++)
		if (strcmp(pcms[i].pcm_path, path) == 0) {
			memcpy(pcm, &pcms[i], sizeof(*pcm));
			found = true;
			break;
		}

	free(pcms);
	return found;
}

static bool print_codecs(const char *path, DBusError *err) {

	DBusMessage *msg = NULL, *rep = NULL;
	bool result = false;
	int count = 0;

	printf("Available codecs:");

	if ((msg = dbus_message_new_method_call(dbus_ctx.ba_service, path,
					BLUEALSA_INTERFACE_PCM, "GetCodecs")) == NULL) {
		dbus_set_error(err, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, err)) == NULL) {
		goto fail;
	}

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(err, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	DBusMessageIter iter_codecs;
	for (dbus_message_iter_recurse(&iter, &iter_codecs);
			dbus_message_iter_get_arg_type(&iter_codecs) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_codecs)) {

		if (dbus_message_iter_get_arg_type(&iter_codecs) != DBUS_TYPE_DICT_ENTRY) {
			dbus_set_error(err, DBUS_ERROR_FAILED, "Message corrupted");
			goto fail;
		}

		DBusMessageIter iter_codecs_entry;
		dbus_message_iter_recurse(&iter_codecs, &iter_codecs_entry);

		if (dbus_message_iter_get_arg_type(&iter_codecs_entry) != DBUS_TYPE_STRING) {
			dbus_set_error(err, DBUS_ERROR_FAILED, "Message corrupted");
			goto fail;
		}

		const char *codec;
		dbus_message_iter_get_basic(&iter_codecs_entry, &codec);
		printf(" %s", codec);
		++count;

		/* Ignore the properties field, get next codec. */
	}
	result = true;

fail:
	if (count == 0)
		printf(" [ Unknown ]");
	printf("\n");

	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return result;
}

static void print_adapters(struct ba_service_props *props) {
	printf("Adapters:");
	for (size_t i = 0; i < ARRAYSIZE(props->adapters); i++)
		if (strlen(props->adapters[i]) > 0)
			printf(" %s", props->adapters[i]);
	printf("\n");
}

static void print_volume(struct ba_pcm *pcm) {
	if (pcm->channels == 2)
		printf("Volume: L: %u R: %u\n", pcm->volume.ch1_volume, pcm->volume.ch2_volume);
	else
		printf("Volume: %u\n", pcm->volume.ch1_volume);
}

static void print_mute(struct ba_pcm *pcm) {
	if (pcm->channels == 2)
		printf("Muted: L: %c R: %c\n",
				pcm->volume.ch1_muted ? 'Y' : 'N', pcm->volume.ch2_muted ? 'Y' : 'N');
	else
		printf("Muted: %c\n", pcm->volume.ch1_muted ? 'Y' : 'N');
}

static int cmd_list_services(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusMessage *msg = NULL, *rep = NULL;
	DBusError err = DBUS_ERROR_INIT;
	int result = EXIT_FAILURE;

	if ((msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
					DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "ListNames")) == NULL) {
		dbus_set_error(&err, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		goto fail;
	}

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(&err, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	DBusMessageIter iter_names;
	for (dbus_message_iter_recurse(&iter, &iter_names);
			dbus_message_iter_get_arg_type(&iter_names) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_names)) {

		if (dbus_message_iter_get_arg_type(&iter_names) != DBUS_TYPE_STRING) {
			char *signature = dbus_message_iter_get_signature(&iter);
			dbus_set_error(&err, DBUS_ERROR_INVALID_SIGNATURE,
					"Incorrect signature: %s != as", signature);
			dbus_free(signature);
			goto fail;
		}

		const char *name;
		dbus_message_iter_get_basic(&iter_names, &name);
		if (strncmp(name, BLUEALSA_SERVICE, sizeof(BLUEALSA_SERVICE) - 1) == 0)
			printf("%s\n", name);

	}

	result = EXIT_SUCCESS;

fail:
	if (dbus_error_is_set(&err))
		cmd_print_error("D-Bus error: %s", err.message);
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return result;
}

static int cmd_list_pcms(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &pcms, &pcms_count, &err)) {
		cmd_print_error("Couldn't get BlueALSA PCM list: %s", err.message);
		return EXIT_FAILURE;
	}

	size_t i;
	for (i = 0; i < pcms_count; i++)
		printf("%s\n", pcms[i].pcm_path);

	free(pcms);
	return EXIT_SUCCESS;
}

static int cmd_status(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	struct ba_service_props props = { 0 };

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_get_props(&dbus_ctx, &props, &err)) {
		cmd_print_error("D-Bus error: %s", err.message);
		return EXIT_FAILURE;
	}

	printf("Service: %s\n", dbus_ctx.ba_service);
	printf("Version: %s\n", props.version);
	print_adapters(&props);

	return EXIT_SUCCESS;
}

static int cmd_info(int argc, char *argv[]) {

	if (argc != 2) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	printf("Device: %s\n", pcm.device_path);
	printf("Sequence: %u\n", pcm.sequence);
	printf("Transport: %s\n", transport_code_to_string(pcm.transport));
	printf("Mode: %s\n", pcm_mode_to_string(pcm.mode));
	printf("Format: %s\n", pcm_format_to_string(pcm.format));
	printf("Channels: %d\n", pcm.channels);
	printf("Sampling: %d Hz\n", pcm.sampling);
	print_codecs(path, &err);
	printf("Selected codec: %s\n", pcm.codec);
	printf("Delay: %#.1f ms\n", (double)pcm.delay / 10);
	printf("SoftVolume: %s\n", pcm.soft_volume ? "Y" : "N");
	print_volume(&pcm);
	print_mute(&pcm);

	if (dbus_error_is_set(&err))
		warn("Unable to read available codecs: %s", err.message);

	return EXIT_SUCCESS;
}

static int cmd_codec(int argc, char *argv[]) {

	if (argc < 2 || argc > 4) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		print_codecs(path, &err);
		printf("Selected codec: %s\n", pcm.codec);
		return EXIT_SUCCESS;
	}

	int result = EXIT_FAILURE;
	const char *codec = argv[2];

	uint8_t data[64];
	uint8_t *data_ptr = NULL;
	size_t len = 0;

	if (argc == 4) {
		len = strlen(argv[3]);
		if (len > sizeof(data) * 2) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "Invalid codec configuration: %s", argv[3]);
			goto fail;
		}
		if ((len = hex2bin(argv[3], data, len)) == -1) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "%s", strerror(errno));
			goto fail;
		}
		data_ptr = data;
	}

	if (!bluealsa_dbus_pcm_select_codec(&dbus_ctx, &pcm, codec, data_ptr, len, &err))
		goto fail;

	result = EXIT_SUCCESS;

fail:
	if (dbus_error_is_set(&err))
		cmd_print_error("Couldn't select BlueALSA PCM Codec: %s", err.message);
	return result;
}

static int cmd_volume(int argc, char *argv[]) {

	if (argc < 2 || argc > 4) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		print_volume(&pcm);
		return EXIT_SUCCESS;
	}

	int vol1, vol2;
	vol1 = vol2 = atoi(argv[2]);
	if (argc == 4)
		vol2 = atoi(argv[3]);

	if (pcm.transport & BA_PCM_TRANSPORT_MASK_A2DP) {
		if (vol1 < 0 || vol1 > 127) {
			cmd_print_error("Invalid volume [0, 127]: %d", vol1);
			return EXIT_FAILURE;
		}
		pcm.volume.ch1_volume = vol1;
		if (pcm.channels == 2) {
			if (vol2 < 0 || vol2 > 127) {
				cmd_print_error("Invalid volume [0, 127]: %d", vol2);
				return EXIT_FAILURE;
			}
			pcm.volume.ch2_volume = vol2;
		}
	}
	else {
		if (vol1 < 0 || vol1 > 15) {
			cmd_print_error("Invalid volume [0, 15]: %d", vol1);
			return EXIT_FAILURE;
		}
		pcm.volume.ch1_volume = vol1;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume loudness update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_mute(int argc, char *argv[]) {

	if (argc < 2 || argc > 4) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		print_mute(&pcm);
		return EXIT_SUCCESS;
	}

	pcm.volume.ch1_muted = pcm.volume.ch2_muted = false;

	if (strcasecmp(argv[2], "y") == 0)
		pcm.volume.ch1_muted = pcm.volume.ch2_muted = true;
	else if (strcasecmp(argv[2], "n") != 0) {
		cmd_print_error("Invalid argument [y|n]: %s", argv[2]);
		return EXIT_FAILURE;
	}

	if (pcm.channels == 2 && argc == 4) {
		if (strcasecmp(argv[3], "y") == 0)
			pcm.volume.ch2_muted = true;
		else if (strcasecmp(argv[3], "n") != 0) {
			cmd_print_error("Invalid argument [y|n]: %s", argv[3]);
			return EXIT_FAILURE;
		}
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume mute update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_softvol(int argc, char *argv[]) {

	if (argc < 2 || argc > 3) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		printf("SoftVolume: %c\n", pcm.soft_volume ? 'Y' : 'N');
		return EXIT_SUCCESS;
	}

	if (strcasecmp(argv[2], "y") == 0)
		pcm.soft_volume = true;
	else if (strcasecmp(argv[2], "n") == 0)
		pcm.soft_volume = false;
	else {
		cmd_print_error("Invalid argument [y|n]: %s", argv[2]);
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_SOFT_VOLUME, &err)) {
		cmd_print_error("SoftVolume update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_open(int argc, char *argv[]) {

	if (argc != 2) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];
	if (!dbus_validate_path(path, NULL)) {
		cmd_print_error("Invalid PCM path: %s", path);
		return EXIT_FAILURE;
	}

	int fd_pcm, fd_pcm_ctrl, input, output;
	size_t len = strlen(path);

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_open_pcm(&dbus_ctx, path, &fd_pcm, &fd_pcm_ctrl, &err)) {
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

static DBusHandlerResult dbus_signal_handler(DBusConnection *conn, DBusMessage *message, void *data) {
	(void)conn;
	(void)data;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	DBusMessageIter iter;
	const char *path;

	if (strcmp(interface, BLUEALSA_INTERFACE_MANAGER) == 0) {

		if (strcmp(signal, "PCMAdded") == 0)
			if (dbus_message_iter_init(message, &iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
				dbus_message_iter_get_basic(&iter, &path);
				printf("PCMAdded %s\n", path);
				return DBUS_HANDLER_RESULT_HANDLED;
			}

		if (strcmp(signal, "PCMRemoved") == 0)
			if (dbus_message_iter_init(message, &iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
				dbus_message_iter_get_basic(&iter, &path);
				printf("PCMRemoved %s\n", path);
				return DBUS_HANDLER_RESULT_HANDLED;
			}

	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int cmd_monitor(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMAdded", NULL);
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMRemoved", NULL);

	if (!dbus_connection_add_filter(dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
		cmd_print_error("Couldn't add D-Bus filter");
		return EXIT_FAILURE;
	}

	while (dbus_connection_read_write_dispatch(dbus_ctx.conn, -1))
		continue;

	return EXIT_SUCCESS;
}

static struct command {
	const char *name;
	int (*func)(int argc, char *arg[]);
	const char *args;
	const char *help;
	unsigned int name_len;
	unsigned int args_len;
} commands[] = {
#define CMD(name, f, args, help) { name, f, args, help, sizeof(name), sizeof(args) }
	CMD("list-services", cmd_list_services, "", "List all BlueALSA services"),
	CMD("list-pcms", cmd_list_pcms, "", "List all BlueALSA PCM paths"),
	CMD("status", cmd_status, "", "Show service runtime properties"),
	CMD("info", cmd_info, "<pcm-path>", "Show PCM properties etc"),
	CMD("codec", cmd_codec, "<pcm-path> [<codec>] [<config>]", "Change codec used by PCM"),
	CMD("volume", cmd_volume, "<pcm-path> [<val>] [<val>]", "Set audio volume"),
	CMD("mute", cmd_mute, "<pcm-path> [y|n] [y|n]", "Mute/unmute audio"),
	CMD("soft-volume", cmd_softvol, "<pcm-path> [y|n]", "Enable/disable SoftVolume property"),
	CMD("monitor", cmd_monitor, "", "Display PCMAdded & PCMRemoved signals"),
	CMD("open", cmd_open, "<pcm-path>", "Transfer raw PCM via stdin or stdout"),
};

static void usage(const char *progname) {

	unsigned int max_len = 0;
	size_t i;

	for (i = 0; i < ARRAYSIZE(commands); i++)
		max_len = MAX(max_len, commands[i].name_len + commands[i].args_len);

	printf("%s - Utility to issue BlueALSA API commands\n", progname);
	printf("\nUsage:\n  %s [options] <command> [command-args]\n", progname);
	printf("\nOptions:\n");
	printf("  -h, --help          Show this help\n");
	printf("  -V, --version       Show version\n");
	printf("  -B, --dbus=NAME     BlueALSA service name suffix\n");
	printf("  -q, --quiet         Do not print any error messages\n");
	printf("\nCommands:\n");
	for (i = 0; i < ARRAYSIZE(commands); i++)
		printf("  %s %-*s%s\n", commands[i].name,
				max_len - commands[i].name_len, commands[i].args,
				commands[i].help);
	printf("\nNotes:\n");
	printf("   1. <pcm-path> must be a valid BlueALSA PCM path as returned by "
	       "the list-pcms command.\n");
	printf("   2. For commands that accept optional arguments, if no such "
	       "argument is given then the current status of the associated "
	       "attribute is printed.\n");
	printf("   3. The codec command requires BlueZ version >= 5.52 "
	       "for SEP support.\n");

}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "+B:Vhq";
	const struct option longopts[] = {
		{"dbus", required_argument, NULL, 'B'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{"quiet", no_argument, NULL, 'q'},
		{ 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			usage(argv[0]);
			return EXIT_SUCCESS;
		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;
		case 'B' /* --dbus=NAME */ :
			snprintf(dbus_ba_service, sizeof(dbus_ba_service), BLUEALSA_SERVICE ".%s", optarg);
			break;
		case 'q' /* --quiet */ :
			quiet = true;
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	log_open(argv[0], false, false);
	dbus_threads_init_default();

	if (argc == optind) {
		cli_print_error("Missing command");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
		cli_print_error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	size_t i;
	for (i = 0; i < ARRAYSIZE(commands); i++)
		if (strcmp(argv[optind], commands[i].name) == 0)
			return commands[i].func(argc - optind, &argv[optind]);

	cli_print_error("Invalid command: %s", argv[optind]);
	return EXIT_FAILURE;
}
