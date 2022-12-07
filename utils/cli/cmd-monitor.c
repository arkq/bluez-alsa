/*
 * BlueALSA - cmd-monitor.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"
#include "shared/log.h"

static bool test_bluealsa_service(const char *name, void *data) {
	bool *result = data;
	if (strcmp(name, BLUEALSA_SERVICE) == 0) {
		*result = true;
		return false;
	}
	*result = false;
	return true;
}

static DBusHandlerResult dbus_signal_handler(DBusConnection *conn, DBusMessage *message, void *data) {
	(void)conn;
	(void)data;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	DBusMessageIter iter;
	if (!dbus_message_iter_init(message, &iter))
		goto fail;

	if (strcmp(interface, DBUS_INTERFACE_OBJECT_MANAGER) == 0) {

		const char *path;
		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
			goto fail;
		dbus_message_iter_get_basic(&iter, &path);

		if (!dbus_message_iter_next(&iter))
			goto fail;

		if (strcmp(signal, "InterfacesAdded") == 0) {

			DBusMessageIter iter_ifaces;
			for (dbus_message_iter_recurse(&iter, &iter_ifaces);
					dbus_message_iter_get_arg_type(&iter_ifaces) != DBUS_TYPE_INVALID;
					dbus_message_iter_next(&iter_ifaces)) {

				DBusMessageIter iter_iface_entry;
				if (dbus_message_iter_get_arg_type(&iter_ifaces) != DBUS_TYPE_DICT_ENTRY)
					goto fail;
				dbus_message_iter_recurse(&iter_ifaces, &iter_iface_entry);

				const char *iface;
				if (dbus_message_iter_get_arg_type(&iter_iface_entry) != DBUS_TYPE_STRING)
					goto fail;
				dbus_message_iter_get_basic(&iter_iface_entry, &iface);

				if (strcmp(iface, BLUEALSA_INTERFACE_PCM) == 0) {

					printf("PCMAdded %s\n", path);

					if (config.verbose) {

						DBusMessageIter iter2;
						if (!dbus_message_iter_init(message, &iter2))
							goto fail;

						struct ba_pcm pcm;
						DBusError err = DBUS_ERROR_INIT;
						if (!bluealsa_dbus_message_iter_get_pcm(&iter2, &err, &pcm)) {
							error("Couldn't read PCM properties: %s", err.message);
							dbus_error_free(&err);
							goto fail;
						}

						cli_print_pcm_properties(&pcm, &err);
						printf("\n");

					}

				}
				else if (strcmp(iface, BLUEALSA_INTERFACE_RFCOMM) == 0) {
					printf("RFCOMMAdded %s\n", path);
				}

			}

			return DBUS_HANDLER_RESULT_HANDLED;
		}
		else if (strcmp(signal, "InterfacesRemoved") == 0) {

			DBusMessageIter iter_ifaces;
			for (dbus_message_iter_recurse(&iter, &iter_ifaces);
					dbus_message_iter_get_arg_type(&iter_ifaces) != DBUS_TYPE_INVALID;
					dbus_message_iter_next(&iter_ifaces)) {

				const char *iface;
				if (dbus_message_iter_get_arg_type(&iter_ifaces) != DBUS_TYPE_STRING)
					goto fail;
				dbus_message_iter_get_basic(&iter_ifaces, &iface);

				if (strcmp(iface, BLUEALSA_INTERFACE_PCM) == 0)
					printf("PCMRemoved %s\n", path);
				else if (strcmp(iface, BLUEALSA_INTERFACE_RFCOMM) == 0)
					printf("RFCOMMRemoved %s\n", path);

			}

			return DBUS_HANDLER_RESULT_HANDLED;
		}

	}
	else if (strcmp(interface, DBUS_INTERFACE_DBUS) == 0) {
		if (strcmp(signal, "NameOwnerChanged") == 0) {

			const char *arg0 = NULL, *arg1 = NULL, *arg2 = NULL;
			if (dbus_message_iter_init(message, &iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
				dbus_message_iter_get_basic(&iter, &arg0);
			else
				goto fail;
			if (dbus_message_iter_next(&iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
				dbus_message_iter_get_basic(&iter, &arg1);
			else
				goto fail;
			if (dbus_message_iter_next(&iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
				dbus_message_iter_get_basic(&iter, &arg2);
			else
				goto fail;

			if (strcmp(arg0, config.dbus.ba_service))
				goto fail;

			if (strlen(arg1) == 0)
				printf("ServiceRunning %s\n", config.dbus.ba_service);
			else if (strlen(arg2) == 0)
				printf("ServiceStopped %s\n", config.dbus.ba_service);
			else
				goto fail;

			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void usage(const char *command) {
	printf("Display PCMAdded & PCMRemoved signals.\n\n");
	cli_print_usage("%s [OPTION]...", command);
	printf("\nOptions:\n"
			"  -h, --help\t\tShow this message and exit\n"
	);
}

static int cmd_monitor_func(int argc, char *argv[]) {

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

	/* Force line buffered output to be sure each event will be flushed
	 * immediately, as this command will most likely be used to write to
	 * a pipe. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	bluealsa_dbus_connection_signal_match_add(&config.dbus,
			config.dbus.ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded",
			"path_namespace='/org/bluealsa'");
	bluealsa_dbus_connection_signal_match_add(&config.dbus,
			config.dbus.ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved",
			"path_namespace='/org/bluealsa'");

	char dbus_args[50];
	snprintf(dbus_args, sizeof(dbus_args), "arg0='%s',arg2=''", config.dbus.ba_service);
	bluealsa_dbus_connection_signal_match_add(&config.dbus,
			DBUS_SERVICE_DBUS, NULL, DBUS_INTERFACE_DBUS, "NameOwnerChanged", dbus_args);
	snprintf(dbus_args, sizeof(dbus_args), "arg0='%s',arg1=''", config.dbus.ba_service);
	bluealsa_dbus_connection_signal_match_add(&config.dbus,
			DBUS_SERVICE_DBUS, NULL, DBUS_INTERFACE_DBUS, "NameOwnerChanged", dbus_args);

	if (!dbus_connection_add_filter(config.dbus.conn, dbus_signal_handler, NULL, NULL)) {
		cmd_print_error("Couldn't add D-Bus filter");
		return EXIT_FAILURE;
	}

	bool running = false;
	DBusError err = DBUS_ERROR_INIT;
	cli_get_ba_services(test_bluealsa_service, &running, &err);
	if (dbus_error_is_set(&err)) {
		cmd_print_error("D-Bus error: %s", err.message);
		return EXIT_FAILURE;
	}

	if (running)
		printf("ServiceRunning %s\n", config.dbus.ba_service);
	else
		printf("ServiceStopped %s\n", config.dbus.ba_service);

	while (dbus_connection_read_write_dispatch(config.dbus.conn, -1))
		continue;

	return EXIT_SUCCESS;
}

const struct cli_command cmd_monitor = {
	"monitor",
	"Display PCMAdded & PCMRemoved signals",
	cmd_monitor_func,
};
