/*
 * BlueALSA - rfcomm.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "shared/ctl-client.h"
#include "shared/log.h"

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

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVi:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "hci", required_argument, NULL, 'i' },
		{ 0, 0, 0, 0 },
	};

	const char *ba_interface = "hci0";
	int status = EXIT_SUCCESS;
	int ba_fd = -1;
	bdaddr_t ba_addr;

	log_open(argv[0], false, false);

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <BT-ADDR>\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -i, --hci=hciX\tHCI device to use\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'i' /* --hci */ :
			ba_interface = optarg;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (optind + 1 != argc)
		goto usage;

	if (str2ba(argv[optind], &ba_addr) != 0) {
		error("Invalid BT device address: %s", argv[optind]);
		goto fail;
	}

	if ((ba_fd = bluealsa_open(ba_interface)) == -1) {
		error("BlueALSA connection failed: %s", strerror(errno));
		goto fail;
	}

	if (isatty(fileno(stdin))) {

		char prompt[17 + 3];
		char *line;

		rl_bind_key('\t', rl_insert);

		sprintf(prompt, "%s> ", argv[optind]);
		while ((line = readline(prompt)) != NULL) {
			char *tmp = strtrim(line);
			if (strlen(tmp) > 0) {
				if (bluealsa_send_rfcomm_command(ba_fd, ba_addr, build_rfcomm_command(tmp)) == -1)
					warn("Couldn't send RFCOMM command: %s", strerror(errno));
				add_history(tmp);
			}
			free(line);
		}

		fprintf(stdout, "\n");

	}
	else {

		char line[256];
		int duration;

		while (fgets(line, sizeof(line), stdin) != NULL) {
			char *tmp = strtrim(line);
			if (strlen(tmp) > 0) {
				if (sscanf(tmp, "%*[Ss]%*[Ll]%*2[Ee]%*[Pp] %d", &duration) == 1) {
					sleep(duration);
					continue;
				}
				if (bluealsa_send_rfcomm_command(ba_fd, ba_addr, build_rfcomm_command(tmp)) == -1)
					warn("Couldn't send RFCOMM command: %s", strerror(errno));
			}
		}

	}

	goto success;

fail:
	status = EXIT_FAILURE;

success:
	if (ba_fd != -1)
		close(ba_fd);
	return status;
}
