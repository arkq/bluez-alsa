/*
 * spawn.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_TEST_INC_SPAWN_H_
#define BLUEALSA_TEST_INC_SPAWN_H_

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#define SPAWN_FLAG_NONE 0
#define SPAWN_FLAG_REDIRECT_STDOUT (1 << 0)
#define SPAWN_FLAG_REDIRECT_STDERR (1 << 1)

struct spawn_process {

	/* PID of newly spawned process */
	pid_t pid;

	/* stdout from the process */
	FILE *f_stdout;
	/* stderr from the process */
	FILE *f_stderr;

	/* async termination */
	unsigned int term_delay_msec;
	pthread_t term_thread;

};

/**
 * Spawn new process using fork() and exec().
 *
 * @param sp Pointer to the structure which will be filled with spawned process
 *   information, i.e. PID, stdout and stderr file descriptors.
 * @param argv List of arguments to be passed to the process. The list shall be
 *   terminated by NULL. The first argument is the name of the executable.
 * @param flags Bitwise OR of the SPAWN_FLAG_* flags.
 * @return On success this function returns 0. Otherwise -1 is returned and
 *   errno is set appropriately. */
int spawn(struct spawn_process *sp, char *argv[], int flags) {

	int pipe_stdout[2] = { -1, -1 };
	int pipe_stderr[2] = { -1, -1 };

	sp->pid = -1;
	sp->f_stderr = NULL;
	sp->f_stdout = NULL;
	sp->term_delay_msec = 0;

	if (flags & SPAWN_FLAG_REDIRECT_STDOUT) {
		if (pipe(pipe_stdout) == -1)
			goto fail;
		if ((sp->f_stdout = fdopen(pipe_stdout[0], "r")) == NULL)
			goto fail;
	}

	if (flags & SPAWN_FLAG_REDIRECT_STDERR) {
		if (pipe(pipe_stderr) == -1)
			goto fail;
		if ((sp->f_stderr = fdopen(pipe_stderr[0], "r")) == NULL)
			goto fail;
	}

	if ((sp->pid = fork()) == 0) {

		if (flags & SPAWN_FLAG_REDIRECT_STDOUT) {
			dup2(pipe_stdout[1], 1);
			fclose(sp->f_stdout);
			close(pipe_stdout[1]);
		}

		if (flags & SPAWN_FLAG_REDIRECT_STDERR) {
			dup2(pipe_stderr[1], 2);
			fclose(sp->f_stderr);
			close(pipe_stderr[1]);
		}

		execv(argv[0], argv);
		return -1;
	}

	close(pipe_stdout[1]);
	close(pipe_stderr[1]);
	return 0;

fail:

	if (sp->f_stdout != NULL)
		fclose(sp->f_stdout);
	else if (pipe_stdout[0] != -1)
		close(pipe_stdout[0]);
	if (pipe_stdout[1] != -1)
		close(pipe_stdout[1]);

	if (sp->f_stderr != NULL)
		fclose(sp->f_stderr);
	else if (pipe_stderr[0] != -1)
		close(pipe_stderr[0]);
	if (pipe_stderr[1] != -1)
		close(pipe_stderr[1]);

	return -1;
}

static void *spawn_timeout_thread(void *arg) {
	struct spawn_process *sp = arg;

	usleep(sp->term_delay_msec * 1000);
	kill(sp->pid, SIGTERM);

	return NULL;
}

int spawn_terminate(struct spawn_process *sp, unsigned int delay_msec) {

	if (delay_msec == 0)
		return kill(sp->pid, SIGTERM);

	sp->term_delay_msec = delay_msec;
	if (pthread_create(&sp->term_thread, NULL, spawn_timeout_thread, sp) != 0)
		return -1;

	return 0;
}

void spawn_close(struct spawn_process *sp) {
	if (sp->term_delay_msec != 0)
		pthread_join(sp->term_thread, NULL);
	if (sp->pid != -1)
		waitpid(sp->pid, NULL, 0);
	if (sp->f_stdout != NULL)
		fclose(sp->f_stdout);
	if (sp->f_stderr != NULL)
		fclose(sp->f_stderr);
}

#endif
