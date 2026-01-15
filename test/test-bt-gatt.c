/*
 * test-bt-gatt.c
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <string.h>

#include <check.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "bluez.h"
#include "bluez-iface.h"
#include "bt-gatt.h"
#include "dbus.h"
#include "utils.h"

#include "inc/check.inc"
#include "mock/service.h"

static BlueZMockService * bluez = NULL;

#define SUCCESS 1
#define FAILURE 2

static bool characteristic_read_callback(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	GVariant * rv = g_variant_new_fixed_byte_array("VALUE", 6);
	/* Return characteristic value to the caller. */
	g_dbus_method_invocation_return_value(inv, g_variant_new_tuple(&rv, 1));
	return true;
}

static int characteristic_notify_fd = -1;
static bool characteristic_acquire_notify_callback(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	int fds[2];
	socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds);
	characteristic_notify_fd = fds[1];
	g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, 512), fd_list);
	return true;
}

static int characteristic_write_fd = -1;
static bool characteristic_acquire_write_callback(
		G_GNUC_UNUSED BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv, G_GNUC_UNUSED void * userdata) {
	int fds[2];
	socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds);
	characteristic_write_fd = fds[1];
	g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hq)", 0, 512), fd_list);
	return true;
}

static BluetoothGATTApplication * gatt_application_new(void) {

	g_autoptr(BluetoothGATTApplication) app;
	ck_assert_ptr_ne(app = bluetooth_gatt_application_new("/app"), NULL);

	g_autoptr(BluetoothGATTService) srv;
	ck_assert_ptr_ne(srv = bluetooth_gatt_service_new("/service0", "0xFFFF", true), NULL);
	bluetooth_gatt_application_add_service(app, srv);

	g_autoptr(BluetoothGATTCharacteristic) chr;
	ck_assert_ptr_ne(chr = bluetooth_gatt_characteristic_new("/char0", "0xFFFF"), NULL);
	bluetooth_gatt_application_add_service_characteristic(app, srv, chr);

	const char * const flags[] = { "read", "write", "notify", NULL };
	bluetooth_gatt_characteristic_set_flags(chr, flags);

	bluetooth_gatt_characteristic_set_read_callback(chr,
			characteristic_read_callback, NULL);
	bluetooth_gatt_characteristic_set_acquire_notify_callback(chr,
			characteristic_acquire_notify_callback, NULL);
	bluetooth_gatt_characteristic_set_acquire_write_callback(chr,
			characteristic_acquire_write_callback, NULL);

	return g_steal_pointer(&app);
}

CK_START_TEST(test_bt_gatt_application) {
	g_autoptr(BluetoothGATTApplication) app = gatt_application_new();
	/* Set the D-Bus connection for the GATT application. */
	bluetooth_gatt_application_set_connection(app, tc_dbus_connection);
	/* Verify that the object manager is created properly. */
	ck_assert_ptr_ne(bluetooth_gatt_application_get_object_manager_server(app), NULL);
} CK_END_TEST

static void register_finish(
		GObject * source, GAsyncResult * result, void * userdata) {
	BluetoothGATTApplication * app = BLUETOOTH_GATT_APPLICATION(source);
	bool ok = bluetooth_gatt_application_register_finish(app, result, NULL);
	/* Notify the test case about the result. */
	g_async_queue_push(userdata, GINT_TO_POINTER(ok ? SUCCESS : FAILURE));
}

CK_START_TEST(test_bt_gatt_application_register) {

	struct ba_adapter * adapter;
	ck_assert_ptr_ne(adapter = ba_adapter_new(MOCK_ADAPTER_ID), NULL);

	g_autoptr(BluetoothGATTApplication) app = gatt_application_new();
	/* Set the D-Bus connection for the GATT application. */
	bluetooth_gatt_application_set_connection(app, tc_dbus_connection);

	g_autoptr(GAsyncQueue) queue = g_async_queue_new();
	bluetooth_gatt_application_register(app, adapter, register_finish, queue);

	/* Wait for the registration to complete and verify that it succeeded. */
	ck_assert_int_eq(GPOINTER_TO_INT(g_async_queue_pop(queue)), SUCCESS);

	/* Verify the UUIDs of registered GATT service and characteristic. */
	g_autofree char * srv_uuid = mock_bluez_service_get_gatt_service_uuid(bluez);
	ck_assert_str_eq(srv_uuid, "0xFFFF");
	g_autofree char * chr_uuid = mock_bluez_service_get_gatt_characteristic_uuid(bluez);
	ck_assert_str_eq(chr_uuid, "0xFFFF");

	ba_adapter_unref(adapter);

} CK_END_TEST

CK_START_TEST(test_bt_gatt_characteristic_callbacks) {

	struct ba_adapter * adapter;
	ck_assert_ptr_ne(adapter = ba_adapter_new(MOCK_ADAPTER_ID), NULL);

	g_autoptr(BluetoothGATTApplication) app = gatt_application_new();
	/* Set the D-Bus connection for the GATT application. */
	bluetooth_gatt_application_set_connection(app, tc_dbus_connection);

	g_autoptr(GAsyncQueue) queue = g_async_queue_new();
	bluetooth_gatt_application_register(app, adapter, register_finish, queue);
	/* Wait for the registration to complete and verify that it succeeded. */
	ck_assert_int_eq(GPOINTER_TO_INT(g_async_queue_pop(queue)), SUCCESS);

	size_t len;
	g_autoptr(GVariant) v = mock_bluez_service_get_gatt_characteristic_value(bluez);
	/* Verify that the read callback returned the expected value. */
	const char * value = g_variant_get_fixed_array(v, &len, sizeof(uint8_t));
	ck_assert_str_eq(value, "VALUE");

	g_autoptr(GIOChannel) notify_ch;
	/* Verify that the notify callback works as expected. */
	notify_ch = mock_bluez_service_acquire_gatt_characteristic_notify_channel(bluez);
	ck_assert_int_ne(characteristic_notify_fd, -1);
	ck_assert_ptr_ne(notify_ch, NULL);

	g_autoptr(GIOChannel) write_ch;
	/* Verify that the write callback works as expected. */
	write_ch = mock_bluez_service_acquire_gatt_characteristic_write_channel(bluez);
	ck_assert_int_ne(characteristic_write_fd, -1);
	ck_assert_ptr_ne(write_ch, NULL);

	ba_adapter_unref(adapter);

} CK_END_TEST

static void tc_setup(void) {
	g_printerr("\n");
	bluez = mock_bluez_service_new();
	g_autoptr(GDBusConnection) conn;
	conn = g_dbus_connection_new_for_address_simple_sync(tc_dbus_address, NULL);
	mock_service_start(bluez, conn);
	/* Set the BlueZ D-Bus unique name used for calls verification. */
	strncpy(bluez_dbus_unique_name, bluez->service.unique_name, sizeof(bluez_dbus_unique_name) - 1);
}

static void tc_teardown(void) {
	mock_service_stop(bluez);
	mock_service_free(g_steal_pointer(&bluez));
}

int main(void) {

	Suite * s = suite_create(__FILE__);
	TCase * tc = tcase_create(__FILE__);
	SRunner * sr = srunner_create(s);

	suite_add_tcase(s, tc);
	tcase_add_checked_fixture(tc, tc_setup_dbus, tc_teardown_dbus);
	tcase_add_checked_fixture(tc, tc_setup_g_main_loop, tc_teardown_g_main_loop);
	tcase_add_checked_fixture(tc, tc_setup, tc_teardown);

	tcase_add_test(tc, test_bt_gatt_application);
	tcase_add_test(tc, test_bt_gatt_application_register);
	tcase_add_test(tc, test_bt_gatt_characteristic_callbacks);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
