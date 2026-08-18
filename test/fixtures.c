/*
 * BlueALSA - fixtures.c
 * SPDX-FileCopyrightText: 2022-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "fixtures.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shared/spawn.h"

/**
 * Full path to the bluealsad-mock executable. */
char bluealsad_mock_path[256] = "bluealsad-mock";

static char * strtrim(char * str) {
	while (isspace(*str))
		str++;
	if (*str == '\0')
		return str;
	char * end = &str[strlen(str) - 1];
	while (end > str && isspace(*end))
		end--;
	end[1] = '\0';
	return str;
}

#define LD_PRELOAD           "LD_PRELOAD"
#define LD_PRELOAD_SANITIZER "LD_PRELOAD_SANITIZER"

int preload(int argc, char * const argv[], const char * filename) {
	(void)argc;

	const char * env_preload;
	if ((env_preload = getenv(LD_PRELOAD)) == NULL)
		env_preload = "";

	const char * env_preload_sanitizer;
	if ((env_preload_sanitizer = getenv(LD_PRELOAD_SANITIZER)) == NULL)
		env_preload_sanitizer = "";

	/* if required library is already preloaded, do nothing */
	if (strstr(env_preload, filename) != NULL)
		return 0;

	fprintf(stderr, "EXECV PRELOAD: %s\n", filename);

	char app[1024];
	char preload[1024];
	char * dir = dirname(strncpy(app, argv[0], sizeof(app) - 1));
	snprintf(preload, sizeof(preload), "%s=%s:%s/%s:%s",
			LD_PRELOAD, env_preload_sanitizer, dir, filename, env_preload);

	putenv(preload);
	return execv(argv[0], argv);
}

struct spawn_bluealsa_data {

	/* The standard error from the BlueALSA server. */
	FILE * f_stderr;

	pthread_mutex_t data_mtx;
	pthread_cond_t data_updated;

	char * dbus_bus_address;
	char * acquired_service_name;
	unsigned int ready_count_a2dp;
	unsigned int ready_count_midi;
	unsigned int ready_count_sco;

};

static void * spawn_bluealsad_mock_stderr_proxy(void * userdata) {
	struct spawn_bluealsa_data * data = userdata;

	char buffer[512];
	while (fgets(buffer, sizeof(buffer), data->f_stderr) != NULL) {

		bool updated = false;
		pthread_mutex_lock(&data->data_mtx);

		char * tmp;
		if ((tmp = strstr(buffer, "DBUS_SYSTEM_BUS_ADDRESS=")) != NULL) {
			data->dbus_bus_address = strtrim(strdup(tmp));
			updated = true;
		}
		else if ((tmp = strstr(buffer, "BLUEALSA_DBUS_SERVICE_NAME=")) != NULL) {
			data->acquired_service_name = strtrim(strdup(&tmp[27]));
			updated = true;
		}
		else if (strstr(buffer, "BLUEALSA_READY=A2DP:") != NULL) {
			data->ready_count_a2dp++;
			updated = true;
		}
		else if (strstr(buffer, "BLUEALSA_READY=MIDI:") != NULL) {
			data->ready_count_midi++;
			updated = true;
		}
		else if (strstr(buffer, "BLUEALSA_READY=SCO:") != NULL) {
			data->ready_count_sco++;
			updated = true;
		}

		pthread_mutex_unlock(&data->data_mtx);

		if (updated)
			pthread_cond_signal(&data->data_updated);

	}

	pthread_mutex_destroy(&data->data_mtx);
	pthread_cond_destroy(&data->data_updated);
	free(data->dbus_bus_address);
	free(data->acquired_service_name);
	fclose(data->f_stderr);
	free(data);
	return NULL;
}

int spawn_bluealsa_mock(struct spawn_process * sp, const char * service,
		int wait_for_ready, ...) {

	/* bus address of D-Bus mock server */
	static char dbus_bus_address[256];

	unsigned int count_a2dp = 0;
	unsigned int count_midi = 0;
	unsigned int count_sco = 0;

	char arg_service[32] = "";
	if (service != NULL)
		sprintf(arg_service, "--dbus=%s", service);

	size_t n = 2;
	char * argv[32] = {
		bluealsad_mock_path,
		arg_service,
	};

	va_list ap;
	va_start(ap, wait_for_ready);

	char * arg;
	while ((arg = va_arg(ap, char *)) != NULL) {

		argv[n++] = arg;
		argv[n] = NULL;

		if (strcmp(arg, "--profile=a2dp-source") == 0)
			count_a2dp += 2;
		if (strcmp(arg, "--profile=a2dp-sink") == 0)
			count_a2dp += 2;
		if (strcmp(arg, "--profile=hfp-ag") == 0)
			count_sco += 1;
		if (strcmp(arg, "--profile=hsp-ag") == 0)
			count_sco += 1;
		if (strcmp(arg, "--profile=midi") == 0)
			count_midi += 1;

	}

	va_end(ap);

	/* NOTE: When testing with ThreadSanitizer on GitHub Actions, we are using
	 *       patched sanitizer library to avoid false positives in cancellation
	 *       cleanup handlers. However, this patch seems to break the sanitizer
	 *       in a way that it does not work correctly with our mock service.
	 *       Therefore, the sanitizer needs to be disabled here. */
	char * const envp[] = { "TSAN_OPTIONS=report_bugs=0", NULL };
	if (spawn(sp, argv, envp, NULL, SPAWN_FLAG_REDIRECT_STDERR) == -1)
		return -1;

	struct spawn_bluealsa_data *data;
	if ((data = calloc(1, sizeof(*data))) == NULL)
		return -1;

	pthread_mutex_init(&data->data_mtx, NULL);
	pthread_cond_init(&data->data_updated, NULL);

	data->f_stderr = sp->f_out;
	sp->f_out = NULL;

	pthread_t tid;
	pthread_create(&tid, NULL, spawn_bluealsad_mock_stderr_proxy, data);
	pthread_detach(tid);

	pthread_mutex_lock(&data->data_mtx);

	/* wait for system bus address */
	while (data->dbus_bus_address == NULL)
		pthread_cond_wait(&data->data_updated, &data->data_mtx);

	strncpy(dbus_bus_address, data->dbus_bus_address,
			sizeof(dbus_bus_address) - 1);
	putenv(dbus_bus_address);

	/* wait for service name acquisition */
	while (data->acquired_service_name == NULL)
		pthread_cond_wait(&data->data_updated, &data->data_mtx);

	while (wait_for_ready && (
				data->ready_count_a2dp < count_a2dp ||
				data->ready_count_midi < count_midi ||
				data->ready_count_sco < count_sco))
		pthread_cond_wait(&data->data_updated, &data->data_mtx);

	pthread_mutex_unlock(&data->data_mtx);

	return 0;
}
