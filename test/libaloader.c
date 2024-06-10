/*
 * aloader.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
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
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <alsa/asoundlib.h>

#include "shared/defs.h"

typedef void *(*dlopen_t)(const char *, int);
static dlopen_t dlopen_orig = NULL;

typedef int (*snd_ctl_open_t)(snd_ctl_t **, const char *, int);
static snd_ctl_open_t snd_ctl_open_orig = NULL;

typedef int (*snd_pcm_open_t)(snd_pcm_t **, const char *, snd_pcm_stream_t, int);
static snd_pcm_open_t snd_pcm_open_orig = NULL;

__attribute__ ((constructor))
static void init(void) {
	*(void **)(&dlopen_orig) = dlsym(RTLD_NEXT, "dlopen");
	*(void **)(&snd_ctl_open_orig) = dlsym(RTLD_NEXT, "snd_ctl_open");
	*(void **)(&snd_pcm_open_orig) = dlsym(RTLD_NEXT, "snd_pcm_open");
}

/**
 * Get build root directory. */
static const char *buildrootdir() {

	static char buffer[1024] = "";

	if (buffer[0] == '\0') {

		char *tmp = strdup(program_invocation_name);
		snprintf(buffer, sizeof(buffer), "%s/..", dirname(tmp));
		free(tmp);

		if (strstr(buffer, "../utils/aplay") != NULL ||
				strstr(buffer, "../utils/cli") != NULL)
			strcat(buffer, "/..");

	}

	return buffer;
}

void *dlopen(const char *filename, int flags) {

	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", buildrootdir());

	if (strstr(filename, "libasound_module_ctl_bluealsa.so") != NULL)
		filename = strcat(tmp, "/src/asound/.libs/libasound_module_ctl_bluealsa.so");
	if (strstr(filename, "libasound_module_pcm_bluealsa.so") != NULL)
		filename = strcat(tmp, "/src/asound/.libs/libasound_module_pcm_bluealsa.so");

	return dlopen_orig(filename, flags);
}

/**
 * Remove any pre-existing bluealsa configuration nodes. */
static void snd_config_ba_cleanup(snd_config_t *config) {

	const char *nodes[] = {
		"defaults.bluealsa",
		"pcm.bluealsa",
		"ctl.bluealsa",
	};

	snd_config_t *node;
	for (size_t i = 0; i < ARRAYSIZE(nodes); i++)
		if (snd_config_search(config, nodes[i], &node) == 0)
			snd_config_delete(node);

}

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode) {

	if (strstr(name, "bluealsa") == NULL)
		return snd_ctl_open_orig(ctl, name, mode);

	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s/src/asound/20-bluealsa.conf", buildrootdir());

	snd_config_t *top = NULL;
	snd_input_t *input = NULL;
	int err;

	if ((err = snd_config_update_ref(&top)) < 0)
		goto fail;
	snd_config_ba_cleanup(top);
	if ((err = snd_input_stdio_open(&input, tmp, "r")) != 0)
		goto fail;
	if ((err = snd_config_load(top, input)) != 0)
		goto fail;
	err = snd_ctl_open_lconf(ctl, name, mode, top);

fail:
	if (top != NULL)
		snd_config_unref(top);
	if (input != NULL)
		snd_input_close(input);
	return err;
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode) {

	if (strstr(name, "bluealsa") == NULL)
		return snd_pcm_open_orig(pcm, name, stream, mode);

	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s/src/asound/20-bluealsa.conf", buildrootdir());

	snd_config_t *top = NULL;
	snd_input_t *input = NULL;
	int err;

	if ((err = snd_config_update_ref(&top)) < 0)
		goto fail;
	snd_config_ba_cleanup(top);
	if ((err = snd_input_stdio_open(&input, tmp, "r")) != 0)
		goto fail;
	if ((err = snd_config_load(top, input)) != 0)
		goto fail;
	err = snd_pcm_open_lconf(pcm, name, stream, mode, top);

fail:
	if (top != NULL)
		snd_config_unref(top);
	if (input != NULL)
		snd_input_close(input);
	return err;
}
