/*
 * BlueALSA - fixtures.h
 * SPDX-FileCopyrightText: 2023-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_TEST_CHECK_SETUP_H_
#define BLUEALSA_TEST_CHECK_SETUP_H_

#include <stdio.h>

#include <check.h>
#include <gio/gio.h>
#include <glib.h>

#include "dbus.h"
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

static GTestDBus * tc_dbus;
static const char * tc_dbus_address;
static GDBusConnection * tc_dbus_connection;

/**
 * Full path to the bluealsad-mock executable. */
extern char bluealsad_mock_path[256];

int preload(int argc, char * const argv[], const char * filename);

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

/**
 * Test case setup function to initialize a mock D-Bus connection. */
static inline void tc_setup_dbus(void) {
	g_test_dbus_up(tc_dbus = g_test_dbus_new(G_TEST_DBUS_NONE));
	tc_dbus_address = g_test_dbus_get_bus_address(tc_dbus);
	tc_dbus_connection = g_dbus_connection_new_for_address_simple_sync(tc_dbus_address, NULL);
}

/**
 * Test case teardown function to free the mock D-Bus connection.
 *
 * Note:
 * Please guarantee that when this function is called, all default main loops
 * are already stopped. This function starts its own main loop and waits for
 * an event to be delivered there. If some other main loop is still running,
 * the event might be delivered to the wrong main loop and this function will
 * wait for 30 seconds before timing out and shutting down the connection. */
static inline void tc_teardown_dbus(void) {
	g_object_unref(tc_dbus_connection);
	g_test_dbus_down(tc_dbus);
	g_object_unref(tc_dbus);
}

static GMainLoop * tc_loop;
static GThread * tc_loop_thread;

static void * tc_g_main_loop_run(void * userdata) {
	g_main_loop_run(userdata);
	return NULL;
}

/**
 * Test case setup function to initialize a new thread with main loop. */
static inline void tc_setup_g_main_loop(void) {
	tc_loop = g_main_loop_new(NULL, FALSE);
	tc_loop_thread = g_thread_new(NULL, tc_g_main_loop_run, tc_loop);
}

/**
 * Test case teardown function to stop and free the main loop thread. */
static inline void tc_teardown_g_main_loop(void) {
	g_main_loop_quit(tc_loop);
	g_main_loop_unref(tc_loop);
	g_thread_join(tc_loop_thread);
}

#endif
