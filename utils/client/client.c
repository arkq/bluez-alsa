/*
 * BlueALSA - client.c
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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"
#include "../dbus.h"


static const char *progname = NULL;
static bool quiet = false;
static char service_name[32] = BLUEALSA_SERVICE;

static struct ba_dbus_ctx dbus_ctx;

static void print_error(const char *format, ...) {
	if (!quiet) {
		va_list ap;
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		fputs("\n", stderr);
		va_end(ap);
	}
}

static void print_error_usage(const char *format, ...) {
	if (!quiet) {
		va_list ap;
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
		fprintf(stderr, "\nTry '%s --help' for more information.\n", progname);
	}
}

static bool check_path(const char *path) {
	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;
	DBusError err = DBUS_ERROR_INIT;
	bool result = false;

	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &pcms, &pcms_count, &err)) {
		print_error("Couldn't get BlueALSA PCM list: %s", err.message);
		return false;
	}

	size_t i;
	for (i = 0; i < pcms_count; i++)
		if (strcmp(pcms[i].pcm_path, path) == 0) {
			result = true;
			break;
		}

	if (!result)
		print_error("Invalid pcm path: %s", path);

	free(pcms);
	return result;
}

static bool get_pcm(const char *path, struct ba_pcm *pcm) {
	struct ba_pcm *pcms = NULL;
	bool found = false;
	size_t pcms_count = 0;
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

static int cmd_list_pcms(int argc, char *argv[]) {
	if (argc > 1) {
		print_error_usage("Too many arguments.");
		return EXIT_FAILURE;
	}

	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;
	DBusError err = DBUS_ERROR_INIT;

	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &pcms, &pcms_count, &err)) {
		print_error("Couldn't get BlueALSA PCM list: %s", err.message);
		return EXIT_FAILURE;
	}

	size_t i;
	for (i = 0; i < pcms_count; i++)
		printf("%s\n", pcms[i].pcm_path);

	free (pcms);

	return EXIT_SUCCESS;
}

static int cmd_get_codecs(int argc, char *argv[]) {
	if (argc != 2) {
		print_error_usage("Invalid arguments.");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	if (!check_path(path)) {
		return EXIT_FAILURE;
	}

	DBusMessage *msg;
	DBusMessage *rep;
	DBusError err = DBUS_ERROR_INIT;
	if ((msg = dbus_message_new_method_call(dbus_ctx.ba_service, path,
					BLUEALSA_INTERFACE_PCM, "GetCodecs")) == NULL) {
		print_error(strerror(ENOMEM));
		return EXIT_FAILURE;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		dbus_message_unref(msg);
		print_error("Couldn't get BlueALSA PCM Codec list: %s", err.message);
		return EXIT_FAILURE;
	}

	int result = EXIT_FAILURE;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		print_error("Empty response message");
		goto fail;
	}

	DBusMessageIter iter_codecs;
	for (dbus_message_iter_recurse(&iter, &iter_codecs);
			dbus_message_iter_get_arg_type(&iter_codecs) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_codecs)) {

		if (dbus_message_iter_get_arg_type(&iter_codecs) != DBUS_TYPE_DICT_ENTRY)  {
			print_error("item is not dict entry");
			goto fail;
		}

		DBusMessageIter iter_codecs_entry;
		dbus_message_iter_recurse(&iter_codecs, &iter_codecs_entry);

		if (dbus_message_iter_get_arg_type(&iter_codecs_entry) != DBUS_TYPE_STRING) {
			print_error("item is not string");
			goto fail;
		}

		const char *codec;
		dbus_message_iter_get_basic(&iter_codecs_entry, &codec);
		printf("%s\n", codec);

		/* Ignore the properties field, get next codec. */
	}
	result = EXIT_SUCCESS;

fail:
	dbus_message_unref(rep);
	dbus_message_unref(msg);

	return result;
}

static int cmd_select_codec(int argc, char *argv[]) {
	if (argc != 3) {
		print_error_usage("Invalid arguments.");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];
	const char *codec = argv[2];

	if (!check_path(path)) {
		return EXIT_FAILURE;
	}

	DBusMessage *msg;
	DBusMessage *rep;
	DBusError err = DBUS_ERROR_INIT;
	if ((msg = dbus_message_new_method_call(dbus_ctx.ba_service, path,
					BLUEALSA_INTERFACE_PCM, "SelectCodec")) == NULL) {
		print_error(strerror(ENOMEM));
		return EXIT_FAILURE;
	}

	DBusMessageIter iter;
	dbus_message_iter_init_append(msg, &iter);

	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &codec)) {
		print_error(strerror(ENOMEM));
		dbus_message_unref(msg);
		return EXIT_FAILURE;
	}

	DBusMessageIter props = DBUS_MESSAGE_ITER_INIT_CLOSED;
	if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &props)) {
		print_error(strerror(ENOMEM));
		dbus_message_unref(msg);
		return EXIT_FAILURE;
	}
	dbus_message_iter_close_container(&iter, &props);

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		dbus_message_unref(msg);
		print_error("Couldn't select BlueALSA PCM Codec: %s", err.message);
		return EXIT_FAILURE;
	}

	dbus_message_unref(rep);
	dbus_message_unref(msg);
	return EXIT_SUCCESS;
}

static int cmd_properties(int argc, char *argv[]) {
	if (argc != 2) {
		print_error_usage("Too many arguments.");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		print_error("Invalid pcm path: %s", path);
		return EXIT_FAILURE;
	}

	/* Get the transport and mode values from the pcm path */
	char *token[6] = { 0 };
	char *tmp = strdup(path);
	char *next = tmp + 1;
	size_t i = 0;
	while (next && i < 6) {
		token[i++] = strsep(&next, "/");
	}

	printf("Device: %s\n", pcm.device_path);

	if (strstr(token[4], "a2dpsrc"))
		printf("Transport: %s\n", "A2DP-source");
	else if (strstr(token[4], "a2dpsink"))
		printf("Transport: %s\n", "A2DP-sink");
	else if (strstr(token[4], "hfpag"))
		printf("Transport: %s\n", "HFP-AG");
	else if (strstr(token[4], "hfphf"))
		printf("Transport: %s\n", "HFP-HF");
	else if (strstr(token[4], "hspag"))
		printf("Transport: %s\n", "HSP-AG");
	else if (strstr(token[4], "hsphs"))
		printf("Transport: %s\n", "HSP-HS");

	if (strstr(token[5], "sink"))
		printf("Mode: %s\n", "sink");
	else if (strstr(token[5], "source"))
		printf("Mode: %s\n", "source");

	switch (pcm.format) {
	case 0x0108:
		printf("Format: %s\n", "U8");
		break;
	case 0x8210:
		printf("Format: %s\n", "S16_LE");
		break;
	case 0x8318:
		printf("Format: %s\n", "S24_3LE");
		break;
	case 0x8418:
		printf("Format: %s\n", "S24_LE");
		break;
	case 0x8420:
		printf("Format: %s\n", "S32_LE");
		break;
	default:
		printf("Format: %s\n", "Unknown");
	}

	printf("Channels: %d\n", pcm.channels);
	printf("Sampling: %d\n", pcm.sampling);
	printf("Codec: %s\n", pcm.codec);
	printf("Delay: %u\n", pcm.delay);
	printf("SoftVolume: %s\n", pcm.soft_volume ? "Y" : "N");

	if (pcm.channels == 2) {
		printf("Volume: L: %u %s R: %u %s\n",
			 pcm.volume.ch1_volume, pcm.volume.ch1_muted ? "(Muted)" : "",
			 pcm.volume.ch2_volume, pcm.volume.ch2_muted ? "(Muted)" : "");
	}
	else {
		printf("Volume: %u %s\n",
			 pcm.volume.ch1_volume, pcm.volume.ch1_muted ? "(Muted)" : "");
	}

	return EXIT_SUCCESS;
}

static int cmd_set_volume(int argc, char *argv[]) {
	if (argc < 3 || argc > 4) {
		print_error_usage("Invalid arguments.");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	int vol1, vol2;
	vol1 = vol2 = atoi(argv[2]);
	if (argc == 4)
		vol2 = atoi(argv[3]);

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		print_error("Invalid pcm path: %s", path);
		return EXIT_FAILURE;
	}

	if (pcm.profile == BA_PCM_PROFILE_A2DP) {
		if (vol1 < 0 || vol1 > 127) {
			print_error("Invalid volume %d ([0 - 127])", vol1);
			return EXIT_FAILURE;
		}
		pcm.volume.ch1_volume = vol1;
		if (pcm.channels == 2) {
			if (vol2 < 0 || vol2 > 127) {
				print_error("Invalid volume %d ([0 - 127])", vol2);
				return EXIT_FAILURE;
			}
			pcm.volume.ch2_volume = vol2;
		}
	}
	else {
		if (vol1 < 0 || vol1 > 15) {
			print_error("Invalid volume %d ([0 - 15])", vol1);
			return EXIT_FAILURE;
		}
		pcm.volume.ch1_volume = vol1;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		print_error("Volume loudness update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_mute(int argc, char *argv[]) {
	if (argc < 3 || argc > 4) {
		print_error_usage("Invalid arguments.");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	bool mute1 = 0, mute2 = 0;

	if (strcasecmp(argv[2], "y") == 0)
		mute1 = true;
	else if (strcasecmp(argv[2], "n") == 0)
		mute1 = false;
	else {
		print_error_usage("Invalid arguments");
		return EXIT_FAILURE;
	}

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		print_error("Invalid pcm path: %s", path);
		return EXIT_FAILURE;
	}

	if (pcm.channels == 2) {
		if (argc == 3)
			mute2 = mute1;
		else {
			if (strcasecmp(argv[3], "y") == 0)
				mute2 = true;
			else if (strcasecmp(argv[3], "n") == 0)
				mute2 = false;
			else {
				print_error_usage("Invalid arguments");
				return EXIT_FAILURE;
			}
		}
	}

	pcm.volume.ch1_muted = mute1;
	if (pcm.channels == 2)
		pcm.volume.ch2_muted = mute2;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		print_error("Volume mute update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_softvol(int argc, char *argv[]) {
	if (argc != 3) {
		print_error_usage("Invalid arguments.");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	if (!check_path(path)) {
		return EXIT_FAILURE;
	}

	bool enable_softvol;
	if (strcasecmp(argv[2], "y") == 0)
		enable_softvol = true;
	else if (strcasecmp(argv[2], "n") == 0)
		enable_softvol = false;
	else {
		print_error_usage("Invalid arguments");
		return EXIT_FAILURE;
	}

	struct ba_pcm pcm = { 0 };
	strncpy(pcm.pcm_path, argv[optind], sizeof(pcm.pcm_path));
	pcm.soft_volume = enable_softvol;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_SOFT_VOLUME, &err)) {
		print_error("SoftVolume update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_open(int argc, char *argv[]) {
	if (argc != 2) {
		print_error_usage("Invalid arguments.");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	if (!check_path(path)) {
		return EXIT_FAILURE;
	}

	int fd_pcm, fd_pcm_ctrl, input, output;
	size_t len = strlen(path);

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_open_pcm(&dbus_ctx, argv[optind], &fd_pcm, &fd_pcm_ctrl, &err)) {
		print_error("Cannot open PCM : %s", err.message);
		return EXIT_FAILURE;
	}

	if (!strcmp(path + len - strlen("source"), "source")) {
		input = fd_pcm;
		output = STDOUT_FILENO;
	}
	else {
		input = STDIN_FILENO;
		output = fd_pcm;
	}

	size_t count;
	char buffer[4096];
	while ((count = read(input, buffer, sizeof(buffer))) > 0) {
		size_t written = 0;
		while (written < count) {
			size_t res = write(output, buffer, count - written);
			if (res <= 0) {
				/* Cannot write any more, so just terminate */
				goto finish;
			}
			written += res;
		}
	}

	if (output == fd_pcm)
		bluealsa_dbus_pcm_ctrl_send_drain(fd_pcm_ctrl, &err);
	else
		/* sleep arbitrary 300ms to allow stdout to drain */
		usleep(300000);

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

	if (strcmp(interface, BLUEALSA_INTERFACE_MANAGER) == 0) {

		if (strcmp(signal, "PCMAdded") == 0)
			if (dbus_message_iter_init(message, &iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
				const char *path;
				dbus_message_iter_get_basic(&iter, &path);
				printf("PCMAdded %s\n", path);
				return DBUS_HANDLER_RESULT_HANDLED;
			}

		if (strcmp(signal, "PCMRemoved") == 0)
			if (dbus_message_iter_init(message, &iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
				const char *path;
				dbus_message_iter_get_basic(&iter, &path);
				printf("PCMRemoved %s\n", path);
				return DBUS_HANDLER_RESULT_HANDLED;
			}
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool monitor_loop_on = true;
static void monitor_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);

	monitor_loop_on = false;
}

static int cmd_monitor(int argc, char *argv[]) {
	if (argc != 1) {
		print_error_usage("Invalid arguments.");
		return EXIT_FAILURE;
	}

	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			service_name, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMAdded", NULL);
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			service_name, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMRemoved", NULL);

	if (!dbus_connection_add_filter(dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
		print_error("Couldn't add D-Bus filter");
		return EXIT_FAILURE;
	}

	struct sigaction sigact = { .sa_handler = monitor_loop_stop };
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	while (monitor_loop_on) {

		struct pollfd pfds[10];
		nfds_t pfds_len = ARRAYSIZE(pfds);

		if (!bluealsa_dbus_connection_poll_fds(&dbus_ctx, pfds, &pfds_len)) {
			print_error("Couldn't get D-Bus connection file descriptors");
			return EXIT_FAILURE;
		}

		if (poll(pfds, pfds_len, -1) == -1 &&
				errno == EINTR)
			continue;

		if (bluealsa_dbus_connection_poll_dispatch(&dbus_ctx, pfds, pfds_len))
			while (dbus_connection_dispatch(dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
				continue;
	}

	return EXIT_SUCCESS;
}

static struct command {
	const char *name;
	int (*func)(int argc, char *arg[]);
	const char *help;
} commands[] = {
	{ "get-codecs", cmd_get_codecs, "PCM_PATH", },
	{ "list-pcms", cmd_list_pcms, "" },
	{ "monitor", cmd_monitor, "" },
	{ "mute", cmd_mute, "PCM_PATH y|n [y|n]" },
	{ "open", cmd_open, "PCM_PATH" },
	{ "properties", cmd_properties, "PCM_PATH" },
	{ "select-codec", cmd_select_codec, "PCM_PATH CODEC" },
	{ "set-volume", cmd_set_volume, "PCM_PATH N [N]" },
	{ "softvol", cmd_softvol, "PCM_PATH y|n" },
};

static void usage(void) {
	int index;
	for (index = 0; index < ARRAYSIZE(commands); index++) {
		printf("%s [options] %s %s\n", progname, commands[index].name, commands[index].help);
	}
	printf("options:\n");
	printf("   -h, --help       Show this help\n");
	printf("   -V, --version    Show version\n");
	printf("   -B, --dbus=NAME  BlueALSA service name suffix\n");
	printf("   -q, --quiet      Do not print any error messages\n");
}

int main(int argc, char *argv[]) {

	const struct option long_options[] = {
		{"dbus",        required_argument, NULL, 'B'},
		{"version",     no_argument, NULL, 'V'},
		{"help",        no_argument, NULL, 'h'},
		{"quiet",       no_argument, NULL, 'q'},
		{ 0 },
	};

	progname = argv[0];

	int c;
    while ((c = getopt_long(argc, argv, "+s:Vh", long_options, NULL)) != -1) {
        switch (c) {
            case 'h' :
                usage();
				return EXIT_SUCCESS;
            case 'V' :
				printf("%s %s\n", progname, PACKAGE_VERSION);
				return EXIT_SUCCESS;
			case 'B' :
				snprintf(service_name, sizeof(service_name), BLUEALSA_SERVICE ".%s", optarg);
				break;
			case 'q' :
				quiet = true;
				break;
		}
	}

    if (optind < argc) {
		int n;
		for (n = 0; n < ARRAYSIZE(commands); n++) {
			if (strcmp(argv[optind], commands[n].name) == 0) {
				DBusError err = DBUS_ERROR_INIT;
				if (!bluealsa_dbus_connection_ctx_init(&dbus_ctx, service_name, &err)) {
					print_error("Couldn't initialize D-Bus context: %s", err.message);
					return EXIT_FAILURE;
				}

				return commands[n].func(argc - optind, &argv[optind]);
			}
		}
	}

	print_error_usage("No valid command specified.");
	return EXIT_FAILURE;

}
