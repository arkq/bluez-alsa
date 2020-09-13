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
#include <string.h>

typedef void *(*dlopen_t)(const char *filename, int flags);

dlopen_t dlopen_orig = NULL;

void *dlopen(const char *filename, int flags) {
	if (dlopen_orig == NULL)
		*(void **)(&dlopen_orig) = dlsym(RTLD_NEXT, __func__);
	if (strstr(filename, "libasound_module_ctl_bluealsa.so") != NULL)
		filename = "../src/asound/.libs/libasound_module_ctl_bluealsa.so";
	if (strstr(filename, "libasound_module_pcm_bluealsa.so") != NULL)
		filename = "../src/asound/.libs/libasound_module_pcm_bluealsa.so";
	return dlopen_orig(filename, flags);
}
