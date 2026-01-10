/*
 * test-ble-midi.c
 * SPDX-FileCopyrightText: 2023-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <check.h>
#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-config.h"
#include "bluez-le-advertisement.h"
#include "dbus.h"
#include "error.h"

#include "inc/check.inc"
#include "mock/service.h"

static BlueZMockService * bluez = NULL;
static GDBusObjectManagerServer * manager = NULL;

#define REGISTER_SUCCESS 1
#define REGISTER_FAILURE 2

static void register_finish(
		GObject * source, GAsyncResult * result, void * userdata) {
	BlueZLEAdvertisement * adv = BLUEZ_LE_ADVERTISEMENT(source);
	bool ok = bluez_le_advertisement_register_finish(adv, result, NULL);
	/* Notify the test case about the result. */
	g_async_queue_push(userdata, GINT_TO_POINTER(ok ? REGISTER_SUCCESS : REGISTER_FAILURE));
}

CK_START_TEST(test_ble_advertisement) {

	struct ba_adapter * adapter;
	ck_assert_ptr_ne(adapter = ba_adapter_new(MOCK_ADAPTER_ID), NULL);

	g_autoptr(BlueZLEAdvertisement) adv;
	ck_assert_ptr_ne(adv = bluez_le_advertisement_new(manager, "0xFFFF", "Foo", "/adv"), NULL);

	g_autoptr(GAsyncQueue) queue = g_async_queue_new();
	bluez_le_advertisement_register(adv, adapter, register_finish, queue);

	/* Wait for the registration to complete and verify that it succeeded. */
	ck_assert_int_eq(GPOINTER_TO_INT(g_async_queue_pop(queue)), REGISTER_SUCCESS);

	/* Verify that the advertisement properties were set correctly. */
	g_autofree char * name = mock_bluez_service_get_advertisement_name(bluez);
	ck_assert_str_eq(name, "Foo");

	bluez_le_advertisement_unregister_sync(adv);
	ba_adapter_unref(adapter);

} CK_END_TEST

CK_START_TEST(test_ble_advertisement_service_data) {

	struct ba_adapter * adapter;
	ck_assert_ptr_ne(adapter = ba_adapter_new(MOCK_ADAPTER_ID), NULL);

	g_autoptr(BlueZLEAdvertisement) adv;
	ck_assert_ptr_ne(adv = bluez_le_advertisement_new(manager, "0xFFFF", "Foo", "/adv"), NULL);

	/* Verify what happens if service data is too big. */
	uint8_t big[128] = { 0 };
	ck_assert_int_eq(bluez_le_advertisement_set_service_data(adv, big, sizeof(big)),
				ERROR_CODE_INVALID_SIZE);

	/* Set service data. */
	uint8_t data[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
	ck_assert_int_eq(bluez_le_advertisement_set_service_data(adv, data, sizeof(data)),
				ERROR_CODE_OK);

	g_autoptr(GAsyncQueue) queue = g_async_queue_new();
	bluez_le_advertisement_register(adv, adapter, register_finish, queue);

	/* Wait for the registration to complete and verify that it succeeded. */
	ck_assert_int_eq(GPOINTER_TO_INT(g_async_queue_pop(queue)), REGISTER_SUCCESS);

	/* Verify that the advertisement service data was set correctly. */
	g_autoptr(GVariant) sd = mock_bluez_service_get_advertisement_service_data(bluez, "0xFFFF");
	ck_assert_ptr_ne(sd, NULL);

	size_t size = 0;
	const uint8_t * sd_bytes = g_variant_get_fixed_array(sd, &size, sizeof(uint8_t));
	ck_assert_int_eq(size, sizeof(data));
	ck_assert_mem_eq(sd_bytes, data, sizeof(data));

	bluez_le_advertisement_unregister_sync(adv);
	ba_adapter_unref(adapter);

} CK_END_TEST

static void tc_setup(void) {

	g_printerr("\n");
	bluez = mock_bluez_service_new();
	g_autoptr(GDBusConnection) conn;
	conn = g_dbus_connection_new_for_address_simple_sync(tc_dbus_address, NULL);
	mock_service_start(bluez, conn);

	config.dbus = tc_dbus_connection;
	manager = g_dbus_object_manager_server_new("/");
	g_dbus_object_manager_server_set_connection(manager, tc_dbus_connection);

}

static void tc_teardown(void) {
	mock_service_stop(bluez);
	mock_service_free(g_steal_pointer(&bluez));
	g_object_unref(g_steal_pointer(&manager));
}

int main(void) {

	Suite * s = suite_create(__FILE__);
	TCase * tc = tcase_create(__FILE__);
	SRunner * sr = srunner_create(s);

	suite_add_tcase(s, tc);
	tcase_add_checked_fixture(tc, tc_setup_dbus, tc_teardown_dbus);
	tcase_add_checked_fixture(tc, tc_setup_g_main_loop, tc_teardown_g_main_loop);
	tcase_add_checked_fixture(tc, tc_setup, tc_teardown);

	tcase_add_test(tc, test_ble_advertisement);
	tcase_add_test(tc, test_ble_advertisement_service_data);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
