/*
 * server.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

static char *bin_path = ".";

/**
 * Spawn bluealsa server mock.
 *
 * @param hci HCI device name.
 * @param timeout Timeout passed to the server-mock.
 * @param source Start A2DP source.
 * @param sink Start A2DP sink.
 * @return PID of the bluealsa server mock. */
pid_t spawn_bluealsa_server(const char *hci, unsigned int timeout, bool source, bool sink) {

	char path[256];
	char arg_device[32];
	char arg_timeout[16];
	pid_t pid;

	sprintf(arg_device, "--device=%s", hci);
	sprintf(arg_timeout, "--timeout=%d", timeout);

	char *argv[] = {
		"server-mock",
		arg_device,
		arg_timeout,
		source ? "--source" : "",
		sink ? "--sink" : "",
		NULL,
	};

	sprintf(path, "%s/server-mock", bin_path);
	/* assert(posix_spawn(&pid, path, NULL, NULL, argv, NULL) == 0); */

	/* XXX: workaround for valgrind-3.13.0 */
	if ((pid = fork()) == 0)
		execv(path, argv);

	usleep(100000);
	return pid;
}
