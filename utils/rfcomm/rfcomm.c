/*
 * BlueALSA - rfcomm.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
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
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "shared/dbus-client.h"
#include "shared/log.h"

static int rfcomm_fd = -1;
static bool main_loop_on = true;

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

	size_t i;
	for (i = 0; i < 6; i++)
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

static char *build_rfcomm_command(const char *cmd) {

	static char command[512];
	bool at;

	command[0] = '\0';
	if (!(at = strncmp(cmd, "AT", 2) == 0))
		strcpy(command, "\r\n");

	strcat(command, cmd);
	strcat(command, "\r");
	if (!at)
		strcat(command, "\n");

	return command;
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

	char *cmd = build_rfcomm_command(line);
	if (write(rfcomm_fd, cmd, strlen(cmd)) == -1)
		warn("Couldn't send RFCOMM command: %s", strerror(errno));

	add_history(line);

}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVB:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "dbus", required_argument, NULL, 'B' },
		{ 0, 0, 0, 0 },
	};

	char dbus_ba_service[32] = BLUEALSA_SERVICE;

	log_open(argv[0], false, false);

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <DEVICE-PATH>\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -B, --dbus=NAME\tBlueALSA service name suffix\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'B' /* --dbus=NAME */ :
			snprintf(dbus_ba_service, sizeof(dbus_ba_service), BLUEALSA_SERVICE ".%s", optarg);
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

	if (!bluealsa_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
		error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	char rfcomm_path[128];
	sprintf(rfcomm_path, "/org/bluealsa/hci%d/dev_%.2X_%.2X_%.2X_%.2X_%.2X_%.2X/rfcomm",
			hci_dev_id, addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]);
	if (!bluealsa_dbus_open_rfcomm(&dbus_ctx, rfcomm_path, &rfcomm_fd, &err)) {
		error("Couldn't open RFCOMM: %s", err.message);
		return EXIT_FAILURE;
	}

	char prompt[32];
	sprintf(prompt, "%.2X:%.2X:%.2X:%.2X:%.2X:%.2X> ",
			addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]);

	rl_bind_key('\t', rl_insert);
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
				rl_replace_line("disconnected", 0);
				rl_message("");
				rl_redisplay();
				break;
			}

			int old_point = rl_point;
			char *old_text = rl_copy_text(0, rl_end);
			rl_save_prompt();
			rl_replace_line("", 0);
			rl_redisplay();

			buffer[ret] = '\0';
			fprintf(stdout, "> %s\n", strtrim(buffer));

			rl_restore_prompt();
			rl_replace_line(old_text, 0);
			rl_point = old_point;
			rl_redisplay();
			free(old_text);

		}

	}

	rl_callback_handler_remove();
	fprintf(stdout, "\n");

	return EXIT_SUCCESS;
}
