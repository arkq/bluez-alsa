/*
 * server.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct spawn_bluealsa_data {

	/* stderr from the BlueALSA server */
	FILE *f_stderr;

	pthread_mutex_t data_mtx;
	pthread_cond_t data_updated;

	char *dbus_bus_address;
	char *acquired_service_name;
	unsigned int ready_count_a2dp;
	unsigned int ready_count_sco;

};

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

static void *spawn_bluealsa_server_stderr_proxy(void *userdata) {

	struct spawn_bluealsa_data *data = userdata;
	char buffer[512];
	char *tmp;

	while (fgets(buffer, sizeof(buffer), data->f_stderr) != NULL) {
		fputs(buffer, stderr);

		pthread_mutex_lock(&data->data_mtx);

		if ((tmp = strstr(buffer, "DBUS_SYSTEM_BUS_ADDRESS=")) != NULL) {
			data->dbus_bus_address = strtrim(strdup(tmp));
			pthread_cond_signal(&data->data_updated);
		}
		else if ((tmp = strstr(buffer, "BLUEALSA_DBUS_SERVICE_NAME=")) != NULL) {
			data->acquired_service_name = strtrim(strdup(&tmp[27]));
			pthread_cond_signal(&data->data_updated);
		}
		else if (strstr(buffer, "BLUEALSA_PCM_READY=A2DP:") != NULL) {
			data->ready_count_a2dp++;
			pthread_cond_signal(&data->data_updated);
		}
		else if (strstr(buffer, "BLUEALSA_PCM_READY=SCO:") != NULL) {
			data->ready_count_sco++;
			pthread_cond_signal(&data->data_updated);
		}

		pthread_mutex_unlock(&data->data_mtx);

	}

	pthread_mutex_destroy(&data->data_mtx);
	pthread_cond_destroy(&data->data_updated);
	free(data->dbus_bus_address);
	fclose(data->f_stderr);
	free(data);
	return NULL;
}

/* path with the bluealsa-mock binary */
char *bluealsa_mock_path = ".";

/**
 * Spawn bluealsa server mock.
 *
 * @param service BlueALSA D-Bus service name.
 * @param timeout Timeout passed to the bluealsa-mock.
 * @param wait_for_ready Block until PCMs are ready.
 * @param fuzzing Enable fuzzing - delayed startup.
 * @param a2dp_source Start A2DP source.
 * @param a2dp_sink Start A2DP sink.
 * @return PID of the bluealsa server mock. */
pid_t spawn_bluealsa_server(const char *service, unsigned int timeout,
	bool wait_for_ready, bool fuzzing, bool a2dp_source, bool a2dp_sink) {

	char arg_service[32] = "";
	if (service != NULL)
		sprintf(arg_service, "--dbus=%s", service);

	char arg_timeout[16];
	sprintf(arg_timeout, "--timeout=%d", timeout);

	char *argv[] = {
		"bluealsa-mock",
		arg_service,
		arg_timeout,
		a2dp_source ? "--a2dp-source" : "",
		a2dp_sink ? "--a2dp-sink" : "",
		fuzzing ? "--fuzzing" : "",
		NULL,
	};

	char path[256];
	sprintf(path, "%s/bluealsa-mock", bluealsa_mock_path);

	int fds[2];
	if (pipe(fds) == -1)
		return -1;

	pid_t pid;
	if ((pid = fork()) == 0) {
		dup2(fds[1], 2);
		close(fds[0]);
		close(fds[1]);
		execv(path, argv);
	}

	close(fds[1]);

	struct spawn_bluealsa_data *data;
	unsigned int count_a2dp = 0;
	unsigned int count_sco = 0;

	if (a2dp_source)
		count_a2dp += 2;
	if (a2dp_sink)
		count_a2dp += 2;

	if ((data = calloc(1, sizeof(*data))) == NULL)
		return -1;

	pthread_mutex_init(&data->data_mtx, NULL);
	pthread_cond_init(&data->data_updated, NULL);
	if ((data->f_stderr = fdopen(fds[0], "r")) == NULL)
		return -1;

	pthread_t tid;
	pthread_create(&tid, NULL, spawn_bluealsa_server_stderr_proxy, data);

	pthread_mutex_lock(&data->data_mtx);

	/* wait for system bus address */
	while (data->dbus_bus_address == NULL)
		pthread_cond_wait(&data->data_updated, &data->data_mtx);
	putenv(data->dbus_bus_address);

	/* wait for service name acquisition */
	while (data->acquired_service_name == NULL)
		pthread_cond_wait(&data->data_updated, &data->data_mtx);

	while (wait_for_ready && (
				data->ready_count_a2dp < count_a2dp ||
				data->ready_count_sco < count_sco))
		pthread_cond_wait(&data->data_updated, &data->data_mtx);

	pthread_mutex_unlock(&data->data_mtx);

	return pid;
}
