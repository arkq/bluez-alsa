/*
 * BlueALSA - fixtures.h
 * SPDX-FileCopyrightText: 2023-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_TEST_FIXTURES_H_
#define BLUEALSA_TEST_FIXTURES_H_

#include <stdio.h>

#include <check.h>
#include <gio/gio.h>
#include <glib.h>

#include "shared/defs.h"
#include "shared/spawn.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SRunner, srunner_free)

/**
 * Wrapper for START_TEST() macro with additional print with test name. */
#define CK_START_TEST(name) START_TEST(name) { \
	fprintf(stderr, "\nTEST: " __FILE__ ":" STRINGIZE(__LINE__) ": " STRINGIZE(name) "\n");

/**
 * Wrapper for END_TEST macro. */
#define CK_END_TEST } END_TEST

/**
 * Preload a shared library and re-execute the current process. */
int preload(int argc, char * const argv[], const char * filename);

/**
 * D-Bus address of the mock server initialized by tc_setup_dbus(). */
extern const char * tc_dbus_address;

/**
 * D-Bus connection to the mock server initialized by tc_setup_dbus(). */
extern GDBusConnection * tc_dbus_connection;

/**
 * Test case setup function to initialize a mock D-Bus connection. */
void tc_setup_dbus(void);

/**
 * Test case teardown function to free the mock D-Bus connection.
 *
 * Note:
 * Please guarantee that when this function is called, all default main loops
 * are already stopped. This function starts its own main loop and waits for
 * an event to be delivered there. If some other main loop is still running,
 * the event might be delivered to the wrong main loop and this function will
 * wait for 30 seconds before timing out and shutting down the connection. */
void tc_teardown_dbus(void);

/**
 * Test case setup function to initialize a new thread with main loop. */
void tc_setup_g_main_loop(void);

/**
 * Test case teardown function to stop and free the main loop thread. */
void tc_teardown_g_main_loop(void);

/**
 * Full path to the bluealsad-mock executable. */
extern char bluealsad_mock_path[256];

/**
 * Spawn BlueALSA mock service.
 *
 * @param process Pointer to the structure which will be filled with spawned
 *   process information, i.e. PID, stdout and stderr file descriptors.
 * @param service BlueALSA D-Bus service name.
 * @param wait_for_ready Block until PCMs are ready.
 * @param ... Additional arguments to be passed to the bluealsad-mock. The list
 *   shall be terminated by NULL.
 * @return On success this function returns 0. Otherwise -1 is returned and
 *  errno is set appropriately. */
int spawn_bluealsa_mock(struct spawn_process * sp, const char * service,
		int wait_for_ready, ...);

#endif
