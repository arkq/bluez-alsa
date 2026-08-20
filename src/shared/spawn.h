/*
 * BlueALSA - spawn.h
 * SPDX-FileCopyrightText: 2022-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_SHARED_SPAWN_H_
#define BLUEALSA_SHARED_SPAWN_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

#define SPAWN_FLAG_NONE 0
#define SPAWN_FLAG_REDIRECT_STDOUT (1 << 0)
#define SPAWN_FLAG_REDIRECT_STDERR (1 << 1)

struct spawn_process {

	/* The PID of newly spawned process. */
	pid_t pid;

	/* The stdout from the process. */
	FILE * f_stdout;
	/* The stderr from the process. */
	FILE * f_stderr;
	/* The input end of the redirection pipe. */
	pthread_barrier_t barrier;
	FILE * f_in;

	/* The redirected output from the process. This stream can be used to read
	 * the output from the process. The streams above are used internally and
	 * should not be used directly. */
	FILE * f_out;

	/* Thread for stdout redirection. */
	pthread_t t_stdout;
	bool t_stdout_created;
	/* Thread for stderr redirection. */
	pthread_t t_stderr;
	bool t_stderr_created;

	/* Async termination. */
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
 * @param envp List of environment variables to be passed to the process. The
 *   list shall be terminated by NULL. If NULL, then the environment from the
 *   parent process will be used.
 * @param f_stdin FILE stream to be used as stdin for the process. If NULL,
 *   then the stdin from the parent process will be used.
 * @param flags Bitwise OR of the SPAWN_FLAG_* flags.
 * @return On success this function returns 0. Otherwise -1 is returned and
 *   errno is set appropriately. */
int spawn(
		struct spawn_process * sp,
		char * const argv[],
		char * const envp[],
		FILE * f_stdin,
		int flags);

/**
 * Terminate spawned process.
 *
 * Please make sure to wait for the process to actually start before calling
 * this function. Otherwise, the SIGTERM may be delivered to the clone of the
 * parent process (after fork() but before exec()) instead of the spawned one.
 * In case of testing with the check framework, the SIGTERM is handled by the
 * signal handler registered by the framework which will lead to termination
 * of the entire test suite.
 *
 * @param sp Pointer to the initialized process information structure.
 * @param delay_msec Delay in milliseconds before sending the SIGTERM to the
 *   process.
 * @return On success this function returns 0. Otherwise -1 is returned and
 *   errno is set appropriately. */
int spawn_terminate(
		struct spawn_process * sp,
		unsigned int delay_msec);

void spawn_close(
		struct spawn_process * sp,
		int * wstatus);

#endif
