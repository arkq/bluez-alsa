/*
 * BlueALSA - cmd-monitor.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
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
#include <strings.h>

#include <dbus/dbus.h>

#include "cli.h"
#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/log.h"

enum {
	PROPERTY_CODEC,
	PROPERTY_RUNNING,
	PROPERTY_SOFTVOL,
	PROPERTY_VOLUME,
};

struct property {
	const char *name;
	bool enabled;
};

static bool monitor_properties = false;
static struct property monitor_properties_set[] = {
	[PROPERTY_CODEC] = { "Codec", false },
	[PROPERTY_RUNNING] = { "Running", false },
	[PROPERTY_SOFTVOL] = { "SoftVolume", false },
	[PROPERTY_VOLUME] = { "Volume", false },
};

static bool test_bluealsa_service(const char *name, void *data) {
	bool *result = data;
	if (strcmp(name, BLUEALSA_SERVICE) == 0) {
		*result = true;
		return false;
	}
	*result = false;
	return true;
}

static dbus_bool_t monitor_dbus_message_iter_get_pcm_props_cb(const char *key,
		DBusMessageIter *value, void *userdata, DBusError *error) {
	const char *path = userdata;

	char type;
	if ((type = dbus_message_iter_get_arg_type(value)) != DBUS_TYPE_VARIANT) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
				"Incorrect property value type: %c != %c", type, DBUS_TYPE_VARIANT);
		return FALSE;
	}

	DBusMessageIter variant;
	dbus_message_iter_recurse(value, &variant);
	type = dbus_message_iter_get_arg_type(&variant);
	char type_expected;

	if (monitor_properties_set[PROPERTY_CODEC].enabled &&
			strcmp(key, monitor_properties_set[PROPERTY_CODEC].name) == 0) {
		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;
		const char *codec;
		dbus_message_iter_get_basic(&variant, &codec);
		printf("PropertyChanged %s Codec %s\n", path, codec);
	}
	else if (monitor_properties_set[PROPERTY_RUNNING].enabled &&
			strcmp(key, monitor_properties_set[PROPERTY_RUNNING].name) == 0) {
		if (type != (type_expected = DBUS_TYPE_BOOLEAN))
			goto fail;
		dbus_bool_t running;
		dbus_message_iter_get_basic(&variant, &running);
		printf("PropertyChanged %s Running %s\n", path, running ? "true" : "false");
	}
	else if (monitor_properties_set[PROPERTY_SOFTVOL].enabled &&
			strcmp(key, monitor_properties_set[PROPERTY_SOFTVOL].name) == 0) {
		if (type != (type_expected = DBUS_TYPE_BOOLEAN))
			goto fail;
		dbus_bool_t softvol;
		dbus_message_iter_get_basic(&variant, &softvol);
		printf("PropertyChanged %s SoftVolume %s\n", path, softvol ? "true" : "false");
	}
	else if (monitor_properties_set[PROPERTY_VOLUME].enabled &&
			strcmp(key, monitor_properties_set[PROPERTY_VOLUME].name) == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_uint16_t volume;
		dbus_message_iter_get_basic(&variant, &volume);
		printf("PropertyChanged %s Volume 0x%.4X\n", path, volume);
	}

	return TRUE;

fail:
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;
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
						if (!dbus_message_iter_get_ba_pcm(&iter2, &err, &pcm)) {
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
	else if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0 &&
			strcmp(signal, "PropertiesChanged") == 0) {

		const char *updated_interface;
		dbus_message_iter_get_basic(&iter, &updated_interface);
		dbus_message_iter_next(&iter);

		if (strcmp(updated_interface, BLUEALSA_INTERFACE_PCM) == 0) {

			DBusError err = DBUS_ERROR_INIT;
			const char *path = dbus_message_get_path(message);
			if (!dbus_message_iter_dict(&iter, &err,
						monitor_dbus_message_iter_get_pcm_props_cb, (void *)path)) {
				error("Unexpected D-Bus signal: %s", err.message);
				dbus_error_free(&err);
				goto fail;
			}

			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool parse_property_list(char *argv[], char *props) {

	if (props == NULL) {
		/* monitor all properties */
		for (size_t i = 0; i < ARRAYSIZE(monitor_properties_set); i++)
			monitor_properties_set[i].enabled = true;
		return true;
	}

	char *prop = strtok(props, ",");
	for (; prop; prop = strtok(NULL, ",")) {

		size_t i;
		for (i = 0; i < ARRAYSIZE(monitor_properties_set); i++) {
			if (strcasecmp(prop, monitor_properties_set[i].name) == 0) {
				monitor_properties_set[i].enabled = true;
				break;
			}
		}

		if (i == ARRAYSIZE(monitor_properties_set)) {
			cmd_print_error("Unknown property '%s'", prop);
			return false;
		}

	}

	return true;
}

static void usage(const char *command) {
	printf("Display D-Bus signals.\n\n");
	cli_print_usage("%s [OPTION]...", command);
	printf("\nOptions:\n"
			"  -h, --help\t\t\tShow this message and exit\n"
			"  -p, --properties[=PROPS]\tShow PCM property changes\n"
	);
}

static int cmd_monitor_func(int argc, char *argv[]) {

	int opt;
	const char *opts = "hp::";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "properties", optional_argument, NULL, 'p' },
		{ 0 },
	};

	opterr = 0;
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			usage(argv[0]);
			return EXIT_SUCCESS;
		case 'p' /* --properties[=PROPS] */ :
			monitor_properties = true;
			if (!parse_property_list(argv, optarg))
				return EXIT_FAILURE;
			break;
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

	ba_dbus_connection_signal_match_add(&config.dbus,
			config.dbus.ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded",
			"path_namespace='/org/bluealsa'");
	ba_dbus_connection_signal_match_add(&config.dbus,
			config.dbus.ba_service, NULL, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved",
			"path_namespace='/org/bluealsa'");

	char dbus_args[50];
	snprintf(dbus_args, sizeof(dbus_args), "arg0='%s',arg2=''", config.dbus.ba_service);
	ba_dbus_connection_signal_match_add(&config.dbus,
			DBUS_SERVICE_DBUS, NULL, DBUS_INTERFACE_DBUS, "NameOwnerChanged", dbus_args);
	snprintf(dbus_args, sizeof(dbus_args), "arg0='%s',arg1=''", config.dbus.ba_service);
	ba_dbus_connection_signal_match_add(&config.dbus,
			DBUS_SERVICE_DBUS, NULL, DBUS_INTERFACE_DBUS, "NameOwnerChanged", dbus_args);

	if (monitor_properties)
		ba_dbus_connection_signal_match_add(&config.dbus, config.dbus.ba_service, NULL,
				DBUS_INTERFACE_PROPERTIES, "PropertiesChanged", "arg0='"BLUEALSA_INTERFACE_PCM"'");

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
	"Display D-Bus signals",
	cmd_monitor_func,
};
