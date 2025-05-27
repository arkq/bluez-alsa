/*
 * BlueALSA - rfcomm.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "shared/dbus-client.h"
#include "shared/dbus-client-rfcomm.h"
#include "shared/log.h"

static int rfcomm_fd = -1;
static bool main_loop_on = true;
static bool input_is_tty = true;
static bool output_is_tty = true;
static bool properties = false;

static int path2hci(const char *path) {
	int id;
	if ((path = strstr(path, "/hci")) == NULL ||
			sscanf(path, "/hci%d/", &id) != 1)
		return -1;
	return id;
}

static int path2ba(const char *path, bdaddr_t *ba) {

	unsigned int x[6];
	if ((path = strstr(path, "/dev_")) == NULL ||
			sscanf(&path[5], "%x_%x_%x_%x_%x_%x",
				&x[5], &x[4], &x[3], &x[2], &x[1], &x[0]) != 6)
		return -1;

	for (size_t i = 0; i < 6; i++)
		ba->b[i] = x[i];

	return 0;
}

static char *strtrim(char *str) {
	while (isspace(*str))
		str++;
	if (*str == '\0')
		return str;
	char *end = &str[strlen(str) - 1];
	while (end > str && isspace(*end))
		end--;
	end[1] = '\0';
	return str;
}

static bool print_properties(struct ba_dbus_ctx *dbus_ctx, const char* path, DBusError *err) {

	struct ba_rfcomm_props props = { 0 };
	if (!ba_dbus_rfcomm_props_get(dbus_ctx, path, &props, err)) {
		ba_dbus_rfcomm_props_free(&props);
		return false;
	}

	printf("Transport: %s\n", props.transport);
	printf("Features:");
	for (size_t i = 0; i < props.features_len; i++)
		printf(" %s", props.features[i]);
	printf("\n");
	printf("Battery: %d\n", props.battery);

	ba_dbus_rfcomm_props_free(&props);

	return true;
}

static char *build_rfcomm_command(char *buffer, size_t size, const char *cmd) {
	bool at = strncmp(cmd, "AT", 2) == 0;
	snprintf(buffer, size, "%s%s%s", at ? "" : "\r\n", cmd, at ? "\r" : "\r\n");
	return buffer;
}

static void rl_callback_handler(char *line) {

	if (line == NULL) {
		rl_callback_handler_remove();
		main_loop_on = false;
		return;
	}

	line = strtrim(line);
	if (strlen(line) == 0)
		return;

	char cmd[512];
	build_rfcomm_command(cmd, sizeof(cmd), line);

	if (write(rfcomm_fd, cmd, strlen(cmd)) == -1)
		warn("Couldn't send RFCOMM command: %s", strerror(errno));

	if (input_is_tty)
		add_history(line);
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVB:p";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "properties", no_argument, NULL, 'p' },
		{ 0, 0, 0, 0 },
	};

	char dbus_ba_service[32] = BLUEALSA_SERVICE;

	log_open(basename(argv[0]), false);

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <DEVICE-PATH>\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -B, --dbus=NAME\tBlueALSA service name suffix\n"
					"  -p, --properties\tprint device properties and exit\n",
					argv[0]);
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

		case 'p' /* --properties */ :
			properties = true;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (optind + 1 != argc)
		goto usage;

	int hci_dev_id;
	bdaddr_t addr;

	if ((hci_dev_id = path2hci(argv[optind])) == -1 ||
			path2ba(argv[optind], &addr) == -1) {
		error("Invalid BT device path: %s", argv[optind]);
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	struct ba_dbus_ctx dbus_ctx;

	if (!ba_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
		error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	char rfcomm_path[128];
	sprintf(rfcomm_path, "/org/bluealsa/hci%d/dev_%.2X_%.2X_%.2X_%.2X_%.2X_%.2X/rfcomm",
			hci_dev_id, addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]);

	if (properties) {
		if (print_properties(&dbus_ctx, rfcomm_path, &err))
			return EXIT_SUCCESS;
		error("D-Bus error: %s", err.message);
		return EXIT_FAILURE;
	}

	if (!ba_dbus_rfcomm_open(&dbus_ctx, rfcomm_path, &rfcomm_fd, &err)) {
		error("Couldn't open RFCOMM: %s", err.message);
		return EXIT_FAILURE;
	}

	input_is_tty = isatty(fileno(stdin));
	output_is_tty = isatty(fileno(stdout));
	const char *prefix = "";
	char prompt[32] = "";

	if (input_is_tty) {
		sprintf(prompt, "%.2X:%.2X:%.2X:%.2X:%.2X:%.2X> ",
			addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]);
		rl_bind_key('\t', rl_insert);
		prefix = "> ";
	}
	else
		/* libreadline echos its input to stdout which is a problem when
		 * stdin is not the terminal. The function rl_tty_set_echoing() is
		 * broken when used with the callback API, so as a workaround we
		 * redirect the (useless) output to /dev/null */
		rl_outstream = fopen("/dev/null", "w");

	if (!output_is_tty)
		/* Force line buffered output to be sure each event will be flushed
		 * immediately. */
		setvbuf(stdout, NULL, _IOLBF, 0);

	rl_callback_handler_install(prompt, rl_callback_handler);

	struct pollfd pfds[] = {
		{ fileno(stdin), POLLIN, 0 },
		{ rfcomm_fd, POLLIN, 0 },
	};

	while (main_loop_on) {

		poll(pfds, 2, -1);

		if (pfds[0].revents & POLLIN)
			rl_callback_read_char();
		else if (pfds[0].revents & POLLHUP)
			break;

		if (pfds[1].revents & POLLIN) {

			char buffer[256];
			ssize_t ret;

			if ((ret = read(rfcomm_fd, buffer, sizeof(buffer) - 1)) <= 0) {
				if (output_is_tty) {
					rl_set_prompt(NULL);
					rl_replace_line("disconnected", 0);
					rl_redisplay();
				}
				break;
			}

			buffer[ret] = '\0';

			char *old_text = NULL;
			int old_point = 0;

			if (output_is_tty) {
				old_point = rl_point;
				old_text = rl_copy_text(0, rl_end);
				rl_save_prompt();
				rl_replace_line("", 0);
				rl_redisplay();
			}

			fprintf(stdout, "%s%s\n", prefix, strtrim(buffer));

			if (output_is_tty) {
				rl_restore_prompt();
				rl_replace_line(old_text, 0);
				rl_point = old_point;
				rl_redisplay();
				free(old_text);
			}

		}

	}

	rl_callback_handler_remove();
	fprintf(stdout, "\n");

	return EXIT_SUCCESS;
}
