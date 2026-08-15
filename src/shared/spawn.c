/*
 * BlueALSA - spawn.h
 * SPDX-FileCopyrightText: 2022-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "spawn.h"

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

struct spawn_forwarder {
	struct spawn_process * sp;
	/* Source stream. */
	FILE * f_source;
	/* Stream for forwarding the output. */
	FILE * f_std_out;
};

static struct spawn_forwarder * spawn_forwarder_new(
		struct spawn_process * sp,
		FILE * f_in,
		FILE * f_out) {

	struct spawn_forwarder * ff;
	if ((ff = malloc(sizeof(*ff))) == NULL)
		return NULL;

	ff->sp = sp;
	ff->f_source = f_in;
	ff->f_std_out = f_out;

	return ff;
}

static void * spawn_forwarder_thread(void * arg) {
	struct spawn_forwarder * ff = arg;

	char buffer[4096];
	while (!feof(ff->f_source))
		if (fgets(buffer, sizeof(buffer), ff->f_source) != NULL) {
			fputs(buffer, ff->f_std_out);
			fputs(buffer, ff->sp->f_in);
			fflush(ff->sp->f_in);
		}

	/* Close the input stream to signal EOF on the output stream. Do it when
	 * all threads have finished reading the output from the spawned process. */
	if (pthread_barrier_wait(&ff->sp->barrier) == PTHREAD_BARRIER_SERIAL_THREAD) {
		fclose(ff->sp->f_in);
		ff->sp->f_in = NULL;
	}

	free(ff);
	return NULL;
}

int spawn(
		struct spawn_process * sp,
		char * argv[],
		FILE * f_stdin,
		int flags) {

	int pipe_stdout[2] = { -1, -1 };
	int pipe_stderr[2] = { -1, -1 };
	int pipe_local[2] = { -1, -1 };

	sp->pid = -1;
	sp->f_stdout = NULL;
	sp->f_stderr = NULL;
	sp->f_in = NULL;
	sp->f_out = NULL;
	sp->t_stdout_created = false;
	sp->t_stderr_created = false;
	sp->term_delay_msec = 0;

	unsigned int count = 1;
	if (flags & SPAWN_FLAG_REDIRECT_STDOUT && flags & SPAWN_FLAG_REDIRECT_STDERR)
		count++;
	pthread_barrier_init(&sp->barrier, NULL, count);

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

		if (f_stdin != NULL)
			dup2(fileno(f_stdin), 0);

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

		return execv(argv[0], argv);
	}

	if (pipe(pipe_local) == -1)
		goto fail;
	if ((sp->f_in = fdopen(pipe_local[1], "w")) == NULL)
		goto fail;
	if ((sp->f_out = fdopen(pipe_local[0], "r")) == NULL)
		goto fail;

	if (flags & SPAWN_FLAG_REDIRECT_STDOUT) {
		struct spawn_forwarder * ff = spawn_forwarder_new(sp, sp->f_stdout, stdout);
		if (pthread_create(&sp->t_stdout, NULL, spawn_forwarder_thread, ff) == 0)
			sp->t_stdout_created = true;
	}

	if (flags & SPAWN_FLAG_REDIRECT_STDERR) {
		struct spawn_forwarder * ff = spawn_forwarder_new(sp, sp->f_stderr, stderr);
		if (pthread_create(&sp->t_stderr, NULL, spawn_forwarder_thread, ff) == 0)
			sp->t_stderr_created = true;
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

	pthread_barrier_destroy(&sp->barrier);

	if (sp->f_in != NULL)
		fclose(sp->f_in);
	else if (pipe_local[0] != -1)
		close(pipe_local[0]);
	if (sp->f_out != NULL)
		fclose(sp->f_out);
	else if (pipe_local[1] != -1)
		close(pipe_local[1]);

	return -1;
}

static void * spawn_timeout_thread(void * arg) {
	struct spawn_process * sp = arg;

	usleep(sp->term_delay_msec * 1000);
	kill(sp->pid, SIGTERM);

	return NULL;
}

int spawn_terminate(
		struct spawn_process * sp,
		unsigned int delay_msec) {

	if (delay_msec == 0)
		return kill(sp->pid, SIGTERM);

	sp->term_delay_msec = delay_msec;
	if (pthread_create(&sp->term_thread, NULL, spawn_timeout_thread, sp) != 0)
		return -1;

	return 0;
}

void spawn_close(
		struct spawn_process * sp,
		int * wstatus) {

	if (sp->term_delay_msec != 0)
		pthread_join(sp->term_thread, NULL);
	if (sp->pid != -1)
		waitpid(sp->pid, wstatus, 0);

	if (sp->t_stdout_created)
		pthread_join(sp->t_stdout, NULL);
	if (sp->t_stderr_created)
		pthread_join(sp->t_stderr, NULL);

	pthread_barrier_destroy(&sp->barrier);

	if (sp->f_stdout != NULL)
		fclose(sp->f_stdout);
	if (sp->f_stderr != NULL)
		fclose(sp->f_stderr);
	if (sp->f_in != NULL)
		fclose(sp->f_in);
	if (sp->f_out != NULL)
		fclose(sp->f_out);

}
