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

#define LD_PRELOAD           "LD_PRELOAD"
#define LD_PRELOAD_SANITIZER "LD_PRELOAD_SANITIZER"

int preload(int argc, char * const argv[], char * const envp[], const char *filename) {
	(void)argc;

	const char *env_preload;
	if ((env_preload = getenv(LD_PRELOAD)) == NULL)
		env_preload = "";

	const char *env_preload_sanitizer;
	if ((env_preload_sanitizer = getenv(LD_PRELOAD_SANITIZER)) == NULL)
		env_preload_sanitizer = "";

	/* if required library is already preloaded, do nothing */
	if (strstr(env_preload, filename) != NULL)
		return 0;

	fprintf(stderr, "EXECV PRELOAD: %s\n", filename);

	char app[1024];
	char preload[1024];
	char *envp2[256] = { preload, NULL };

	char *dir = dirname(strncpy(app, argv[0], sizeof(app) - 1));
	snprintf(preload, sizeof(preload), "%s=%s:%s/%s:%s",
			LD_PRELOAD, env_preload_sanitizer, dir, filename, env_preload);

	size_t i = 1, j = 0;
	while (i < ARRAYSIZE(envp2) - 1 && envp[j] != NULL)
		envp2[i++] = envp[j++];
	if (i == ARRAYSIZE(envp2) - 1 && envp[j] != NULL)
		fprintf(stderr, "WARNING: Couldn't forward ENV variables\n");

	return execve(argv[0], argv, envp2);
}
