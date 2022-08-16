/*
 * preload.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
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

#include "shared/defs.h"

#define LDPRELOAD "LD_PRELOAD"
int preload(int argc, char * const argv[], char * const envp[], const char *filename) {
	(void)argc;

	char *tmp;
	if ((tmp = getenv(LDPRELOAD)) != NULL &&
			strstr(tmp, filename) != NULL)
		return 0;

	fprintf(stderr, "EXECV PRELOAD: %s\n", filename);

	char app[1024];
	char preload[1024];
	char *envp2[128] = { preload, NULL };

	char *dir = dirname(strncpy(app, argv[0], sizeof(app) - 1));
	snprintf(preload, sizeof(preload), "%s=%s/%s:%s",
			LDPRELOAD, dir, filename, tmp != NULL ? tmp : "");

	size_t i = 1, j = 0;
	while (i < ARRAYSIZE(envp2) - 1 && envp[j] != NULL)
		envp2[i++] = envp[j++];
	if (i == ARRAYSIZE(envp2) - 1 && envp[j] != NULL)
		fprintf(stderr, "WARNING: Couldn't forward ENV variables\n");

	return execve(argv[0], argv, envp2);
}
