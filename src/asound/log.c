/*
 * BlueALSA - asound/log.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "asound/log.h"

#if SND_LIB_VERSION < 0x01020F

#include <pthread.h>
#include <stdlib.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int __snd_log_level = -1;

void snd_log_init(void) {

	pthread_mutex_lock(&mutex);

	if (__snd_log_level != -1)
		goto finish;

	const char * env_log_level = getenv("BLUEALSA_LOG_LEVEL");
	if (env_log_level && *env_log_level) {
		if (strcmp(env_log_level, "warning") == 0)
			__snd_log_level = BA_LOG_WARN;
		else if (strcmp(env_log_level, "info") == 0)
			__snd_log_level = BA_LOG_INFO;
		else if (strcmp(env_log_level, "debug") == 0)
			__snd_log_level = BA_LOG_DEBUG;
		else
			__snd_log_level = BA_LOG_ERR;
	}

finish:
	pthread_mutex_unlock(&mutex);
}

#endif
