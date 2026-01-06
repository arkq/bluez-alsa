/*
 * test-dbus.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
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
	GDBusObjectManagerServer * manager;
	GAsyncQueue * queue;
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
	g_async_queue_push(((FooServer *)userdata)->queue, GINT_TO_POINTER(1));
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
static FooServer * foo_server_new(GDBusConnection * conn) {

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
		.get_property = dbus_foo_server_get_property,
		.set_property = dbus_foo_server_set_property,
	};

	FooServer * fs = g_new0(FooServer, 1);
	fs->queue = g_async_queue_new();

	g_autoptr (GDBusObjectSkeleton) skeleton = g_dbus_object_skeleton_new("/foo");
	g_autoptr (OrgExampleFooSkeleton) ifs = org_example_foo_skeleton_new(&vtable, fs, NULL);
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(ifs));

	fs->manager = g_dbus_object_manager_server_new("/");
	g_dbus_object_manager_server_export(fs->manager, skeleton);

	g_bus_own_name_on_connection(conn, "org.example", G_BUS_NAME_OWNER_FLAGS_NONE,
			dbus_foo_server_name_acquired, NULL, fs, NULL);
	g_async_queue_pop(fs->queue);

	g_dbus_object_manager_server_set_connection(fs->manager, conn);

	return fs;
}

/**
 * Free FooServer instance. */
static void foo_server_free(FooServer * fs) {
	g_object_unref(fs->manager);
	g_async_queue_unref(fs->queue);
	g_free(fs);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FooServer, foo_server_free)

CK_START_TEST(test_dbus_dispatch_method_call) {

	g_autoptr(FooServer) server;
	ck_assert_ptr_nonnull((server = foo_server_new(tc_dbus_connection)));

	/* Call not-handled method. */
	g_autoptr(GError) err = NULL;
	g_autoptr(GDBusMessage) msg = g_dbus_message_new_method_call(
			"org.example", "/foo", "org.example.Foo", "Boom");
	g_autoptr(GDBusMessage) rep = g_dbus_connection_send_message_with_reply_sync(
			tc_dbus_connection, msg, G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, NULL);
	ck_assert_uint_eq(g_dbus_message_to_gerror(rep, &err), TRUE);
	ck_assert_uint_eq(err->code, G_DBUS_ERROR_UNKNOWN_METHOD);

	/* Call handled method and wait for reply. */
	g_autoptr(GDBusMessage) msg2 = g_dbus_message_new_method_call(
			"org.example", "/foo", "org.example.Foo", "Ping");
	g_autoptr(GDBusMessage) rep2 = g_dbus_connection_send_message_with_reply_sync(
			tc_dbus_connection, msg2, G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, NULL);

	/* Check if the right handler was called. */
	ck_assert_uint_eq(server->called_method_error, false);
	ck_assert_uint_eq(server->called_method_ping, true);

} CK_END_TEST

static void dbus_signal_handler(G_GNUC_UNUSED GDBusConnection *conn,
		G_GNUC_UNUSED const char *sender, const char *path, const char *interface,
		const char *signal, G_GNUC_UNUSED GVariant *params, void *userdata) {
	ck_assert_str_eq(path, "/foo");
	ck_assert_str_eq(interface, "org.freedesktop.DBus.Properties");
	ck_assert_str_eq(signal, "PropertiesChanged");
	g_async_queue_push(userdata, GINT_TO_POINTER(1));
}

CK_START_TEST(test_g_dbus_connection_emit_properties_changed) {

	g_autoptr(GAsyncQueue) queue = g_async_queue_new();
	/* Subscribe for signal, so we can check the emit function. */
	g_dbus_connection_signal_subscribe(tc_dbus_connection, NULL, NULL, "PropertiesChanged",
			NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, dbus_signal_handler, queue, NULL);

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&props, "{sv}", "Bar", g_variant_new_boolean(FALSE));

	bool ok = g_dbus_connection_emit_properties_changed(tc_dbus_connection, "/foo",
			"org.example.Foo", g_variant_builder_end(&props), NULL, NULL);
	ck_assert_uint_eq(ok, true);

	/* Wait for signal to be received. The signal handler will check
	 * if the signal was received with the right parameters. */
	g_async_queue_pop(queue);

} CK_END_TEST

CK_START_TEST(test_g_dbus_get_unique_name_sync) {

	g_autoptr(FooServer) server;
	ck_assert_ptr_nonnull((server = foo_server_new(tc_dbus_connection)));

	/* Try to get unique name for non-existing service. */
	ck_assert_ptr_null(g_dbus_get_unique_name_sync(tc_dbus_connection, "org.nonexistent"));

	g_autofree char * name;
	/* Try to get unique name for existing service. */
	ck_assert_ptr_nonnull((name = g_dbus_get_unique_name_sync(tc_dbus_connection, "org.example")));

} CK_END_TEST

CK_START_TEST(test_g_dbus_get_managed_objects_sync) {

	g_autoptr(FooServer) server;
	ck_assert_ptr_nonnull((server = foo_server_new(tc_dbus_connection)));

	/* Try to get managed objects from non-managed path. */
	g_autoptr(GError) err = NULL;
	ck_assert_ptr_null(g_dbus_get_managed_objects_sync(tc_dbus_connection, "org.example", "/test", &err));
	ck_assert_uint_eq(err->code, G_DBUS_ERROR_UNKNOWN_METHOD);

	/* Try to get managed objects from managed path. */
	g_autoptr(GVariantIter) objects = g_dbus_get_managed_objects_sync(
			tc_dbus_connection, "org.example", "/", NULL);
	ck_assert_ptr_nonnull(objects);

} CK_END_TEST

CK_START_TEST(test_g_dbus_get_properties_sync) {

	g_autoptr(FooServer) server;
	ck_assert_ptr_nonnull((server = foo_server_new(tc_dbus_connection)));

	g_autoptr(GError) err = NULL;
	/* Try to get properties on non-existing interface. */
	ck_assert_ptr_null(g_dbus_get_properties_sync(tc_dbus_connection,
			"org.example", "/foo", "org.example.Foo5", &err));
	ck_assert_int_eq(err->code, G_DBUS_ERROR_INVALID_ARGS);

	/* Try to get properties on existing interface. */
	g_autoptr(GVariantIter) props = g_dbus_get_properties_sync(
			tc_dbus_connection, "org.example", "/foo", "org.example.Foo", NULL);
	ck_assert_ptr_nonnull(props);

} CK_END_TEST

typedef struct {
	GVariant * property;
	GAsyncQueue * queue;
} GetPropertyData;

static GetPropertyData * get_property_data_new(void) {
	GetPropertyData * data = g_new0(GetPropertyData, 1);
	data->queue = g_async_queue_new();
	return data;
}

static void get_property_data_free(GetPropertyData * data) {
	if (data->property != NULL)
		g_variant_unref(data->property);
	g_async_queue_unref(data->queue);
	g_free(data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GetPropertyData, get_property_data_free)

static void dbus_get_property_finish(GObject * source, GAsyncResult * result, void * userdata) {
	GetPropertyData * data = userdata;
	data->property = g_dbus_get_property_finish(G_DBUS_CONNECTION(source), result, NULL);
	g_async_queue_push(data->queue, GINT_TO_POINTER(1));
}

CK_START_TEST(test_g_dbus_get_property) {

	g_autoptr(FooServer) server;
	ck_assert_ptr_nonnull((server = foo_server_new(tc_dbus_connection)));

	g_autoptr(GetPropertyData) data1 = get_property_data_new();
	/* Try to get non-existing property. */
	g_dbus_get_property(tc_dbus_connection, "org.example", "/foo", "org.example.Foo", "No",
			dbus_get_property_finish, data1);
	g_async_queue_pop(data1->queue);

	ck_assert_ptr_null(data1->property);
	ck_assert_uint_eq(server->called_get_property, false);

	g_autoptr(GetPropertyData) data2 = get_property_data_new();
	/* Try to get existing property. */
	g_dbus_get_property(tc_dbus_connection, "org.example", "/foo", "org.example.Foo", "Bar",
			dbus_get_property_finish, data2);
	g_async_queue_pop(data2->queue);

	ck_assert_ptr_nonnull(data2->property);
	ck_assert_uint_eq(g_variant_get_boolean(data2->property), false);
	ck_assert_uint_eq(server->called_get_property, true);

} CK_END_TEST

CK_START_TEST(test_g_dbus_set_property_sync) {

	g_autoptr(FooServer) server;
	ck_assert_ptr_nonnull((server = foo_server_new(tc_dbus_connection)));

	g_autoptr(GError) err = NULL;
	/* Try to set non-existing property. */
	ck_assert_uint_eq(g_dbus_set_property_sync(tc_dbus_connection, "org.example", "/foo",
			"org.example.Foo", "No", g_variant_new_boolean(TRUE), &err), false);
	ck_assert_uint_eq(server->called_set_property, false);
	ck_assert_int_eq(err->code, G_DBUS_ERROR_INVALID_ARGS);

	/* Try to set existing property. */
	ck_assert_int_eq(g_dbus_set_property_sync(tc_dbus_connection, "org.example", "/foo",
			"org.example.Foo", "Bar", g_variant_new_boolean(TRUE), NULL), true);
	ck_assert_uint_eq(server->called_set_property, true);

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);
	tcase_add_checked_fixture(tc, tc_setup_dbus, tc_teardown_dbus);
	tcase_add_checked_fixture(tc, tc_setup_g_main_loop, tc_teardown_g_main_loop);

	tcase_add_test(tc, test_dbus_dispatch_method_call);
	tcase_add_test(tc, test_g_dbus_connection_emit_properties_changed);
	tcase_add_test(tc, test_g_dbus_get_unique_name_sync);
	tcase_add_test(tc, test_g_dbus_get_managed_objects_sync);
	tcase_add_test(tc, test_g_dbus_get_properties_sync);
	tcase_add_test(tc, test_g_dbus_get_property);
	tcase_add_test(tc, test_g_dbus_set_property_sync);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
