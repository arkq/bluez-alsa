/*
 * preload.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LDPRELOAD "LD_PRELOAD"
int preload(int argc, char *argv[], const char *filename) {
	(void)argc;

	char *tmp;
	if ((tmp = getenv(LDPRELOAD)) != NULL &&
			strstr(tmp, filename) != NULL)
		return 0;

	char preload[1024];
	char *envp[] = { preload, NULL };

	char app[1024];
	char *dir = dirname(strncpy(app, argv[0], sizeof(app) - 1));

	sprintf(preload, "%s=%s/%s:", LDPRELOAD, dir, filename);
	if (tmp != NULL)
		strcat(preload, tmp);

	fprintf(stderr, "EXECV PRELOAD: %s\n", filename);
	return execve(argv[0], argv, envp);
}
