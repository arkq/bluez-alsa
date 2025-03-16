/*
 * BlueALSA - bluealsactl/main.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <getopt.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "bluealsactl.h"
#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/log.h"

/* Initialize global configuration variable. */
struct bactl_config config = {
	.quiet = false,
	.verbose = 0,
};

static const char *transport_code_to_string(int transport_code) {
	switch (transport_code) {
	case BA_PCM_TRANSPORT_A2DP_SOURCE:
		return "A2DP-source";
	case BA_PCM_TRANSPORT_A2DP_SINK:
		return "A2DP-sink";
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

static void print_pcm_codec(const struct ba_pcm_codec *codec) {

	printf("%s", codec->name);

	if (config.verbose && codec->data_len > 0) {
		printf(":");
		for (size_t i = 0; i < codec->data_len; i++)
			printf("%.2x", codec->data[i]);
	}

	if (config.verbose >= 2) {

		printf(" [channels:");
		for (size_t i = 0; i < ARRAYSIZE(codec->channels) && codec->channels[i] != 0; i++)
			printf(" %u", codec->channels[i]);
		printf("]");

		printf(" [rate:");
		for (size_t i = 0; i < ARRAYSIZE(codec->rates) && codec->rates[i] != 0; i++)
			printf(" %u", codec->rates[i]);
		printf("]");

	}

}

void bactl_get_ba_services(bactl_get_ba_services_cb func, void *data, DBusError *err) {

	DBusMessage *msg = NULL, *rep = NULL;

	if ((msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
					DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "ListNames")) == NULL) {
		dbus_set_error_const(err, DBUS_ERROR_NO_MEMORY, NULL);
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(config.dbus.conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, err)) == NULL) {
		goto fail;
	}

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(err, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	DBusMessageIter iter_names;
	for (dbus_message_iter_recurse(&iter, &iter_names);
			dbus_message_iter_get_arg_type(&iter_names) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_names)) {

		if (dbus_message_iter_get_arg_type(&iter_names) != DBUS_TYPE_STRING) {
			char *signature = dbus_message_iter_get_signature(&iter);
			dbus_set_error(err, DBUS_ERROR_INVALID_SIGNATURE,
					"Incorrect signature: %s != as", signature);
			dbus_free(signature);
			goto fail;
		}

		const char *name;
		dbus_message_iter_get_basic(&iter_names, &name);
		if (!func(name, data)) {
			break;
		}

	}

fail:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
}

bool bactl_get_ba_pcm(const char *path, struct ba_pcm *pcm, DBusError *err) {

	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;

	if (!dbus_validate_path(path, err))
		return false;

	if (!ba_dbus_pcm_get_all(&config.dbus, &pcms, &pcms_count, err))
		return false;

	bool found = false;
	for (size_t i = 0; i < pcms_count; i++)
		if (strcmp(pcms[i].pcm_path, path) == 0) {
			memcpy(pcm, &pcms[i], sizeof(*pcm));
			found = true;
			break;
		}

	free(pcms);

	if (!found)
		dbus_set_error(err, DBUS_ERROR_UNKNOWN_OBJECT,
				"Object path not found: '%s'", path);
	return found;
}

bool bactl_parse_common_options(int opt) {
	switch (opt) {
	case 'q' /* --quiet */ :
		config.quiet = true;
		return true;
	case 'v' /* --verbose */ :
		config.verbose++;
		return true;
	default:
		return false;
	}
}

bool bactl_parse_value_on_off(const char *value, bool *out) {

	static const char * const value_on[] = { "on", "yes", "true", "y", "1" };
	static const char * const value_off[] = { "off", "no", "false", "n", "0" };

	for (size_t i = 0; i < ARRAYSIZE(value_on); i++)
		if (strcasecmp(value, value_on[i]) == 0) {
			*out = true;
			return true;
		}

	for (size_t i = 0; i < ARRAYSIZE(value_off); i++)
		if (strcasecmp(value, value_off[i]) == 0) {
			*out = false;
			return true;
		}

	return false;
}

void bactl_print_adapters(const struct ba_service_props *props) {
	printf("Adapters:");
	for (size_t i = 0; i < props->adapters_len; i++)
		printf(" %s", props->adapters[i]);
	printf("\n");
}

void bactl_print_profiles_and_codecs(const struct ba_service_props *props) {
	printf("Profiles:\n");
	for (size_t i = 0; i < props->profiles_len; i++) {
		printf("  %-11s :", props->profiles[i]);
		size_t len = strlen(props->profiles[i]);
		for (size_t ii = 0; ii < props->codecs_len; ii++)
			if (strncmp(props->codecs[ii], props->profiles[i], len) == 0)
				printf(" %s", &props->codecs[ii][len + 1]);
		printf("\n");
	}
}

void bactl_print_pcm_available_codecs(const struct ba_pcm *pcm, DBusError *err) {

	printf("Available codecs:");

	struct ba_pcm_codecs codecs = { 0 };
	if (!ba_dbus_pcm_codecs_get(&config.dbus, pcm->pcm_path, &codecs, err))
		goto fail;

	for (size_t i = 0; i < codecs.codecs_len; i++) {
		printf("%s", config.verbose >= 2 ? "\n\t" : " ");
		print_pcm_codec(&codecs.codecs[i]);
	}

	ba_dbus_pcm_codecs_free(&codecs);

fail:
	if (codecs.codecs_len == 0)
		printf(" [ Unknown ]");
	printf("\n");
}

void bactl_print_pcm_selected_codec(const struct ba_pcm *pcm) {
	printf("Selected codec:%s", config.verbose >= 2 ? "\n\t" : " ");
	print_pcm_codec(&pcm->codec);
	printf("\n");
}

void bactl_print_pcm_delay(const struct ba_pcm *pcm) {
	printf("Delay: %#.1f ms\n", pcm->delay / 10.0);
}

void bactl_print_pcm_client_delay(const struct ba_pcm *pcm) {
	printf("ClientDelay: %#.1f ms\n", pcm->client_delay / 10.0);
}

void bactl_print_pcm_soft_volume(const struct ba_pcm *pcm) {
	printf("SoftVolume: %s\n", pcm->soft_volume ? "true" : "false");
}

void bactl_print_pcm_channel_map(const struct ba_pcm *pcm) {
	printf("ChannelMap:");
	for (size_t i = 0; i < pcm->channels; i++)
		printf(" %s", pcm->channel_map[i]);
	printf("\n");
}

void bactl_print_pcm_volume(const struct ba_pcm *pcm) {
	printf("Volume:");
	for (size_t i = 0; i < pcm->channels; i++)
		printf(" %u", pcm->volume[i].volume);
	printf("\n");
}

void bactl_print_pcm_mute(const struct ba_pcm *pcm) {
	printf("Mute:");
	for (size_t i = 0; i < pcm->channels; i++)
		printf(" %s", pcm->volume[i].muted ? "on" : "off");
	printf("\n");
}

void bactl_print_pcm_properties(const struct ba_pcm *pcm, DBusError *err) {
	printf("Device: %s\n", pcm->device_path);
	printf("Sequence: %u\n", pcm->sequence);
	printf("Transport: %s\n", transport_code_to_string(pcm->transport));
	printf("Mode: %s\n", pcm_mode_to_string(pcm->mode));
	printf("Running: %s\n", pcm->running ? "true" : "false");
	printf("Format: %s\n", pcm_format_to_string(pcm->format));
	printf("Channels: %d\n", pcm->channels);
	bactl_print_pcm_channel_map(pcm);
	printf("Rate: %d Hz\n", pcm->rate);
	bactl_print_pcm_available_codecs(pcm, err);
	bactl_print_pcm_selected_codec(pcm);
	bactl_print_pcm_delay(pcm);
	bactl_print_pcm_client_delay(pcm);
	bactl_print_pcm_soft_volume(pcm);
	bactl_print_pcm_volume(pcm);
	bactl_print_pcm_mute(pcm);
}

static const char *progname = NULL;
void bactl_print_usage(const char *format, ...) {

	char usage[256];
	va_list va;

	va_start(va, format);
	vsnprintf(usage, sizeof(usage), format, va);
	va_end(va);

	printf("Usage:\n  %s %s\n", progname, usage);
}

extern const struct bactl_command cmd_list_services;
extern const struct bactl_command cmd_list_pcms;
extern const struct bactl_command cmd_status;
extern const struct bactl_command cmd_info;
extern const struct bactl_command cmd_codec;
extern const struct bactl_command cmd_client_delay;
extern const struct bactl_command cmd_monitor;
extern const struct bactl_command cmd_mute;
extern const struct bactl_command cmd_open;
extern const struct bactl_command cmd_softvol;
extern const struct bactl_command cmd_volume;

static const struct bactl_command *commands[] = {
	&cmd_list_services,
	&cmd_list_pcms,
	&cmd_status,
	&cmd_info,
	&cmd_codec,
	&cmd_client_delay,
	&cmd_volume,
	&cmd_mute,
	&cmd_softvol,
	&cmd_monitor,
	&cmd_open,
};

static void usage(const char *name) {

	size_t command_name_max_len = 0;
	for (size_t i = 0; i < ARRAYSIZE(commands); i++) {
		size_t len = strlen(commands[i]->name);
		command_name_max_len = MAX(command_name_max_len, len);
	}

	printf("%s - Utility to issue BlueALSA API commands\n\n", name);
	bactl_print_usage("[OPTION]... COMMAND [COMMAND-ARGS]");
	printf("\nOptions:\n");
	printf("  -h, --help          Show this message and exit\n");
	printf("  -V, --version       Show version and exit\n");
	printf("  -B, --dbus=NAME     BlueALSA service name suffix\n");
	printf("  -q, --quiet         Do not print any error messages\n");
	printf("  -v, --verbose       Increase output verbosity\n");
	printf("\nCommands:\n");
	for (size_t i = 0; i < ARRAYSIZE(commands); i++)
		printf("  %-*s  %s\n", (int)(command_name_max_len),
				commands[i]->name, commands[i]->description);

}

int main(int argc, char *argv[]) {

	progname = argv[0];

	int opt;
	const char *opts = "+hqvB:V";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "version", no_argument, NULL, 'V' },
		{ 0 },
	};

	char dbus_ba_service[32] = BLUEALSA_SERVICE;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1) {
		if (bactl_parse_common_options(opt))
			continue;
		switch (opt) {
		case 'h' /* --help */ :
			usage(argv[0]);
			return EXIT_SUCCESS;
		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;
		case 'B' /* --dbus=NAME */ :
			snprintf(dbus_ba_service, sizeof(dbus_ba_service), BLUEALSA_SERVICE ".%s", optarg);
			if (!dbus_validate_bus_name(dbus_ba_service, NULL)) {
				error("Invalid BlueALSA D-Bus service name: %s", dbus_ba_service);
				return EXIT_FAILURE;
			}
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	log_open(basename(argv[0]), false);
	dbus_threads_init_default();

	DBusError err = DBUS_ERROR_INIT;
	if (!ba_dbus_connection_ctx_init(&config.dbus, dbus_ba_service, &err)) {
		bactl_print_error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc == 0) {
		/* show "status" information by default */
		char *status_argv[] = { "status", NULL };
		return cmd_status.func(1, status_argv);
	}

	for (size_t i = 0; i < ARRAYSIZE(commands); i++)
		if (strcmp(argv[0], commands[i]->name) == 0)
			return commands[i]->func(argc, argv);

	bactl_print_error("Invalid command: %s", argv[0]);
	return EXIT_FAILURE;
}
