/*
 * service.c
 * SPDX-FileCopyrightText: 2023-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "service.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "shared/log.h"

/**
 * Simple read callback which drains the input buffer. */
int channel_drain_callback(GIOChannel * ch, GIOCondition cond,
		G_GNUC_UNUSED void * userdata) {

	char buffer[1024];

	switch (cond) {
	case G_IO_HUP:
	case G_IO_ERR:
		return G_SOURCE_REMOVE;
	case G_IO_IN:
		/* Just drain the input buffer. */
		g_io_channel_read_chars(ch, buffer, sizeof(buffer), NULL, NULL);
		g_printerr("#");
		/* fall-through */
	default:
		return G_SOURCE_CONTINUE;
	}

}

static void name_acquired(G_GNUC_UNUSED GDBusConnection * conn,
		G_GNUC_UNUSED const char * name, void * userdata) {
	mock_service_ready(userdata);
}

static void * mock_loop_run(void * userdata) {
	MockService * service = userdata;
	debug("Starting service loop: %s", service->name);

	g_autoptr(GMainContext) context = g_main_context_new();
	service->_loop = g_main_loop_new(context, FALSE);
	g_main_context_push_thread_default(context);

	g_assert((service->_id = g_bus_own_name_on_connection(service->_conn,
					service->name, G_BUS_NAME_OWNER_FLAGS_NONE,
					service->name_acquired_cb, service->name_lost_cb,
					userdata, NULL)) != 0);

	g_main_loop_run(service->_loop);

	g_main_context_pop_thread_default(context);
	return NULL;
}

void mock_service_start(MockService * service, GDBusConnection * conn) {

	if (service->name_acquired_cb == NULL)
		/* Set default name acquired callback if not provided. */
		service->name_acquired_cb = name_acquired;

	service->_conn = g_object_ref(conn);
	service->_ready = g_async_queue_new();

	service->_thread = g_thread_new(service->name, mock_loop_run, service);
	g_async_queue_pop(service->_ready);

}

void mock_service_ready(MockService * service) {
	g_async_queue_push(service->_ready, GINT_TO_POINTER(1));
}

void mock_service_stop(MockService * service) {
	g_bus_unown_name(service->_id);
	g_main_loop_quit(service->_loop);
	g_main_loop_unref(service->_loop);
	g_thread_join(service->_thread);
	g_async_queue_unref(service->_ready);
	g_object_unref(service->_conn);
}
