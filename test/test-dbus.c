/*
 * test-dbus.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdbool.h>
#include <stddef.h>

#include <check.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "dbus.h"

#include "test-dbus-iface.h"
#include "inc/check.inc"

typedef struct {
	GMutex mtx;
	GCond cond;
	unsigned int count;
} SyncBarrier;

void sync_barrier_init(SyncBarrier *sb, unsigned int count) {
	g_mutex_init(&sb->mtx);
	g_cond_init(&sb->cond);
	sb->count = count;
}

void sync_barrier_signal(SyncBarrier *sb) {
	g_mutex_lock(&sb->mtx);
	sb->count--;
	g_mutex_unlock(&sb->mtx);
	g_cond_signal(&sb->cond);
}

void sync_barrier_wait(SyncBarrier *sb) {
	g_mutex_lock(&sb->mtx);
	while (sb->count > 0)
		g_cond_wait(&sb->cond, &sb->mtx);
	g_mutex_unlock(&sb->mtx);
}

void sync_barrier_free(SyncBarrier *sb) {
	g_mutex_clear(&sb->mtx);
	g_cond_clear(&sb->cond);
}

typedef struct {
	GDBusConnection *conn;
	GTestDBus *dbus;
	GMainLoop *loop;
	GThread *thread;
} GTestDBusConnection;

static void *main_loop_run(void *userdata) {
	g_main_loop_run((GMainLoop *)userdata);
	return NULL;
}

/**
 * Get new D-Bus connection on the mock bus. */
static GTestDBusConnection *test_dbus_connection_new(void) {
	GTestDBusConnection *conn = g_new0(GTestDBusConnection, 1);
	conn->dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
	conn->loop = g_main_loop_new(NULL, FALSE);
	conn->thread = g_thread_new(NULL, main_loop_run, conn->loop);
	g_test_dbus_up(conn->dbus);
	conn->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	return conn;
}

/**
 * Free D-Bus connection and stop the mock bus. */
static void test_dbus_connection_free(GTestDBusConnection *conn) {
	/* terminate main loop */
	g_main_loop_quit(conn->loop);
	g_main_loop_unref(conn->loop);
	g_thread_join(conn->thread);
	/* stop the mock bus */
	g_object_unref(conn->conn);
	g_test_dbus_down(conn->dbus);
	g_free(conn);
}

typedef struct {
	GDBusObjectManagerServer *manager;
	SyncBarrier acquired_barrier;
	bool called_method_error;
	bool called_method_ping;
	bool called_get_property;
	bool called_set_property;
	bool bar;
} FooServer;

static void dbus_foo_server_method_call_error(GDBusMethodInvocation *inv,
		void *userdata) {
	((FooServer *)userdata)->called_method_error = true;
	g_dbus_method_invocation_return_value(inv, NULL);
}

static void dbus_foo_server_method_call_ping(GDBusMethodInvocation *inv,
		void *userdata) {
	((FooServer *)userdata)->called_method_ping = true;
	g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *dbus_foo_server_get_property(G_GNUC_UNUSED const char *property,
		G_GNUC_UNUSED GError **error, void *userdata) {
	((FooServer *)userdata)->called_get_property = true;
	return g_variant_new_boolean(((FooServer *)userdata)->bar);
}

static bool dbus_foo_server_set_property(G_GNUC_UNUSED const char *property,
		G_GNUC_UNUSED GVariant *value, G_GNUC_UNUSED GError **error, void *userdata) {
	((FooServer *)userdata)->called_set_property = true;
	return true;
}

static void dbus_foo_server_name_acquired(G_GNUC_UNUSED GDBusConnection *conn,
		G_GNUC_UNUSED const char *name, void *userdata) {
	sync_barrier_signal(&((FooServer *)userdata)->acquired_barrier);
}

static const GDBusMethodCallDispatcher dispatchers[] = {
	{ .method = "Ping",
		.interface = "org.example.FooSpecial",
		.handler = dbus_foo_server_method_call_error },
	{ .method = "Ping",
		.path = "/foo/internal",
		.handler = dbus_foo_server_method_call_error },
	{ .method = "Ping",
		.sender = "org.example.threat",
		.handler = dbus_foo_server_method_call_error },
	{ .method = "Ping",
		.interface = "org.example.Foo",
		.handler = dbus_foo_server_method_call_ping },
	{ 0 },
};

/**
 * Create new FooServer instance on the given D-Bus connection. */
static FooServer *dbus_foo_server_new(GDBusConnection *conn) {

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
		.get_property = dbus_foo_server_get_property,
		.set_property = dbus_foo_server_set_property,
	};

	FooServer *fs = g_new0(FooServer, 1);
	sync_barrier_init(&fs->acquired_barrier, 1);

	GDBusObjectSkeleton *skeleton = g_dbus_object_skeleton_new("/foo");
	OrgExampleFooSkeleton *ifs = org_example_foo_skeleton_new(&vtable, fs, NULL);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(ifs));
	g_object_unref(ifs);

	fs->manager = g_dbus_object_manager_server_new("/");
	g_dbus_object_manager_server_export(fs->manager, skeleton);
	g_object_unref(skeleton);

	g_bus_own_name_on_connection(conn, "org.example", G_BUS_NAME_OWNER_FLAGS_NONE,
			dbus_foo_server_name_acquired, NULL, fs, NULL);

	sync_barrier_wait(&fs->acquired_barrier);

	g_dbus_object_manager_server_set_connection(fs->manager, conn);

	return fs;
}

/**
 * Free FooServer instance. */
static void dbus_foo_server_free(FooServer *fs) {
	g_object_unref(fs->manager);
	sync_barrier_free(&fs->acquired_barrier);
	g_free(fs);
}

CK_START_TEST(test_dbus_dispatch_method_call) {

	FooServer *server;
	GTestDBusConnection *tc;
	GDBusMessage *msg, *rep;
	GError *err = NULL;

	ck_assert_ptr_nonnull((tc = test_dbus_connection_new()));
	ck_assert_ptr_nonnull((server = dbus_foo_server_new(tc->conn)));

	/* call not-handled method */
	msg = g_dbus_message_new_method_call("org.example", "/foo", "org.example.Foo", "Boom");
	rep = g_dbus_connection_send_message_with_reply_sync(tc->conn, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, NULL);
	ck_assert_uint_eq(g_dbus_message_to_gerror(rep, &err), TRUE);
	ck_assert_uint_eq(err->code, G_DBUS_ERROR_UNKNOWN_METHOD);
	g_object_unref(msg);
	g_object_unref(rep);
	g_error_free(err);

	/* call handled method and wait for reply */
	msg = g_dbus_message_new_method_call("org.example", "/foo", "org.example.Foo", "Ping");
	rep = g_dbus_connection_send_message_with_reply_sync(tc->conn, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, NULL);
	g_object_unref(msg);
	g_object_unref(rep);

	/* check if the right handler was called */
	ck_assert_uint_eq(server->called_method_error, false);
	ck_assert_uint_eq(server->called_method_ping, true);

	dbus_foo_server_free(server);
	test_dbus_connection_free(tc);

} CK_END_TEST

static void dbus_signal_handler(G_GNUC_UNUSED GDBusConnection *conn,
		G_GNUC_UNUSED const char *sender, const char *path, const char *interface,
		const char *signal, G_GNUC_UNUSED GVariant *params, void *userdata) {
	ck_assert_str_eq(path, "/foo");
	ck_assert_str_eq(interface, "org.freedesktop.DBus.Properties");
	ck_assert_str_eq(signal, "PropertiesChanged");
	sync_barrier_signal(userdata);
}

CK_START_TEST(test_g_dbus_connection_emit_properties_changed) {

	SyncBarrier sb;
	sync_barrier_init(&sb, 1);

	GTestDBusConnection *tc;
	ck_assert_ptr_nonnull((tc = test_dbus_connection_new()));

	/* Subscribe for signal, so we can check the emit function. */
	g_dbus_connection_signal_subscribe(tc->conn, NULL, NULL, "PropertiesChanged",
			NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, dbus_signal_handler, &sb, NULL);

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&props, "{sv}", "Bar", g_variant_new_boolean(FALSE));

	bool ok = g_dbus_connection_emit_properties_changed(tc->conn, "/foo",
			"org.example.Foo", g_variant_builder_end(&props), NULL, NULL);
	ck_assert_uint_eq(ok, true);

	/* Wait for signal to be received. The signal handler will check
	 * if the signal was received with the right parameters. */
	sync_barrier_wait(&sb);

	test_dbus_connection_free(tc);
	sync_barrier_free(&sb);

} CK_END_TEST

CK_START_TEST(test_g_dbus_get_managed_objects) {

	FooServer *server;
	GTestDBusConnection *tc;

	ck_assert_ptr_nonnull((tc = test_dbus_connection_new()));
	ck_assert_ptr_nonnull((server = dbus_foo_server_new(tc->conn)));

	/* try to get managed objects from non-managed path */
	GError *err = NULL;
	ck_assert_ptr_null(g_dbus_get_managed_objects(tc->conn, "org.example", "/test", &err));
	ck_assert_uint_eq(err->code, G_DBUS_ERROR_UNKNOWN_METHOD);
	g_error_free(err);

	GVariantIter *objects;
	/* try to get managed objects from managed path */
	objects = g_dbus_get_managed_objects(tc->conn, "org.example", "/", NULL);
	ck_assert_ptr_nonnull(objects);
	g_variant_iter_free(objects);

	dbus_foo_server_free(server);
	test_dbus_connection_free(tc);

} CK_END_TEST

CK_START_TEST(test_g_dbus_get_properties) {

	FooServer *server;
	GTestDBusConnection *tc;

	ck_assert_ptr_nonnull((tc = test_dbus_connection_new()));
	ck_assert_ptr_nonnull((server = dbus_foo_server_new(tc->conn)));

	GError *err = NULL;
	/* try to get properties on non-existing interface */
	ck_assert_ptr_null(g_dbus_get_properties(tc->conn,
			"org.example", "/foo", "org.example.Foo5", &err));
	ck_assert_int_eq(err->code, G_DBUS_ERROR_INVALID_ARGS);
	g_error_free(err);

	/* try to get properties on existing interface */
	GVariantIter *props = g_dbus_get_properties(tc->conn,
			"org.example", "/foo", "org.example.Foo", NULL);
	ck_assert_ptr_nonnull(props);
	g_variant_iter_free(props);

	dbus_foo_server_free(server);
	test_dbus_connection_free(tc);

} CK_END_TEST

CK_START_TEST(test_g_dbus_get_property) {

	FooServer *server;
	GTestDBusConnection *tc;

	ck_assert_ptr_nonnull((tc = test_dbus_connection_new()));
	ck_assert_ptr_nonnull((server = dbus_foo_server_new(tc->conn)));

	GError *err = NULL;
	/* try to get non-existing property */
	ck_assert_ptr_null(g_dbus_get_property(tc->conn,
			"org.example", "/foo", "org.example.Foo", "No", &err));
	ck_assert_uint_eq(server->called_get_property, false);
	ck_assert_int_eq(err->code, G_DBUS_ERROR_INVALID_ARGS);
	g_error_free(err);

	/* try to get existing property */
	GVariant *prop = g_dbus_get_property(tc->conn,
			"org.example", "/foo", "org.example.Foo", "Bar", NULL);
	ck_assert_uint_eq(server->called_get_property, true);
	ck_assert_ptr_nonnull(prop);
	ck_assert_uint_eq(g_variant_get_boolean(prop), false);

	dbus_foo_server_free(server);
	test_dbus_connection_free(tc);

} CK_END_TEST

CK_START_TEST(test_g_dbus_set_property) {

	FooServer *server;
	GTestDBusConnection *tc;

	ck_assert_ptr_nonnull((tc = test_dbus_connection_new()));
	ck_assert_ptr_nonnull((server = dbus_foo_server_new(tc->conn)));

	GError *err = NULL;
	/* try to set non-existing property */
	ck_assert_uint_eq(g_dbus_set_property(tc->conn, "org.example", "/foo",
			"org.example.Foo", "No", g_variant_new_boolean(TRUE), &err), false);
	ck_assert_uint_eq(server->called_set_property, false);
	ck_assert_int_eq(err->code, G_DBUS_ERROR_INVALID_ARGS);
	g_error_free(err);

	/* try to set existing property */
	ck_assert_int_eq(g_dbus_set_property(tc->conn, "org.example", "/foo",
			"org.example.Foo", "Bar", g_variant_new_boolean(TRUE), NULL), true);
	ck_assert_uint_eq(server->called_set_property, true);

	dbus_foo_server_free(server);
	test_dbus_connection_free(tc);

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_dbus_dispatch_method_call);
	tcase_add_test(tc, test_g_dbus_connection_emit_properties_changed);
	tcase_add_test(tc, test_g_dbus_get_managed_objects);
	tcase_add_test(tc, test_g_dbus_get_properties);
	tcase_add_test(tc, test_g_dbus_get_property);
	tcase_add_test(tc, test_g_dbus_set_property);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
