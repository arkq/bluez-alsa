/*
 * preload.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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

#define LD_PRELOAD           "LD_PRELOAD"
#define LD_PRELOAD_SANITIZER "LD_PRELOAD_SANITIZER"

int preload(int argc, char * const argv[], const char *filename) {
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
	char *dir = dirname(strncpy(app, argv[0], sizeof(app) - 1));
	snprintf(preload, sizeof(preload), "%s=%s:%s/%s:%s",
			LD_PRELOAD, env_preload_sanitizer, dir, filename, env_preload);

	putenv(preload);
	return execv(argv[0], argv);
}
