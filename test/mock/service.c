/*
 * BlueALSA - service.c
 * SPDX-FileCopyrightText: 2023-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "service.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "dbus.h"
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

	service->_context = g_main_context_new();
	service->_loop = g_main_loop_new(service->_context, FALSE);
	g_main_context_push_thread_default(service->_context);

	g_assert((service->_id = g_bus_own_name_on_connection(service->_conn,
					service->name, G_BUS_NAME_OWNER_FLAGS_NONE,
					service->name_acquired_cb, service->name_lost_cb,
					userdata, NULL)) != 0);

	g_main_loop_run(service->_loop);

	g_main_context_pop_thread_default(service->_context);
	return NULL;
}

static int mock_loop_quit(void * userdata) {
	g_main_loop_quit(userdata);
	return G_SOURCE_REMOVE;
}

void mock_service_start(void * service, GDBusConnection * conn) {
	MockService * srv = service;

	if (srv->name_acquired_cb == NULL)
		/* Set default name acquired callback if not provided. */
		srv->name_acquired_cb = name_acquired;

	srv->_conn = g_object_ref(conn);
	srv->_ready = g_async_queue_new();

	srv->_thread = g_thread_new(srv->name, mock_loop_run, service);
	g_async_queue_pop(srv->_ready);

	/* Get the unique bus name assigned to the service. */
	srv->unique_name = g_dbus_get_unique_name_sync(conn, srv->name);

}

void mock_service_ready(void * service) {
	MockService * srv = service;
	g_async_queue_push(srv->_ready, GINT_TO_POINTER(1));
}

void mock_service_stop(void * service) {
	MockService * srv = service;

	g_bus_unown_name(srv->_id);
	srv->stop(service);

	/* Quit the loop after any pending idle sources have been dispatched. */
	g_autoptr(GSource) source = g_idle_source_new();
	g_source_set_priority(source, G_PRIORITY_LOW);
	g_source_set_callback(source, mock_loop_quit, srv->_loop, NULL);
	g_source_attach(source, srv->_context);

	g_thread_join(srv->_thread);
	g_free(g_steal_pointer(&srv->unique_name));


}

void mock_service_free(void * service) {
	MockService * srv = service;

	g_main_loop_unref(srv->_loop);
	g_main_context_unref(srv->_context);
	g_async_queue_unref(srv->_ready);
	g_object_unref(srv->_conn);

	srv->free(service);

}
