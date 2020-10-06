/*
 * aloader.c
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

#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*dlopen_t)(const char *filename, int flags);

static dlopen_t dlopen_orig = NULL;
static char program_invocation_path[1024];

void *dlopen(const char *filename, int flags) {

	if (dlopen_orig == NULL) {

		*(void **)(&dlopen_orig) = dlsym(RTLD_NEXT, __func__);

		char *tmp = strdup(program_invocation_name);
		strcpy(program_invocation_path, dirname(tmp));
		free(tmp);

	}

	char buffer[1024 + 128];
	strcpy(buffer, program_invocation_path);

	if (strstr(filename, "libasound_module_ctl_bluealsa.so") != NULL)
		filename = strcat(buffer, "/../src/asound/.libs/libasound_module_ctl_bluealsa.so");
	if (strstr(filename, "libasound_module_pcm_bluealsa.so") != NULL)
		filename = strcat(buffer, "/../src/asound/.libs/libasound_module_pcm_bluealsa.so");

	return dlopen_orig(filename, flags);
}
