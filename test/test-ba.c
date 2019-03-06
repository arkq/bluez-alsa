/*
 * test-ba.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <check.h>

#include "../src/ba-adapter.c"
#include "../src/ba-device.c"
#include "../src/ba-transport.c"
#include "../src/bluealsa.c"
#include "../src/utils.c"
#include "../src/shared/defs.h"
#include "../src/shared/log.c"

struct ba_ctl *bluealsa_ctl_init(struct ba_adapter *a) {
	(void)a; return (struct ba_ctl *)0xDEAD; }
void bluealsa_ctl_free(struct ba_ctl *ctl) { (void)ctl; }
int bluealsa_ctl_send_event(struct ba_ctl *ctl,
		enum ba_event ev, const bdaddr_t *addr, uint8_t type) {
	(void)ctl; (void)ev; (void)addr; (void)type; return 0; }
void *io_thread_a2dp_source_sbc(void *arg) { (void)arg; return NULL; }
void *io_thread_a2dp_sink_sbc(void *arg) { (void)arg; return NULL; }
void *io_thread_a2dp_source_aac(void *arg) { (void)arg; return NULL; }
void *io_thread_a2dp_sink_aac(void *arg) { (void)arg; return NULL; }
void *io_thread_a2dp_source_aptx(void *arg) { (void)arg; return NULL; }
void *io_thread_a2dp_source_ldac(void *arg) { (void)arg; return NULL; }
void *io_thread_sco(void *arg) { (void)arg; return NULL; }
void *rfcomm_thread(void *arg) { (void)arg; return NULL; }

START_TEST(test_ba_adapter) {

	struct ba_adapter *a;

	ck_assert_ptr_ne(a = ba_adapter_new(0, NULL), NULL);
	ck_assert_str_eq(a->hci_name, "hci0");
	ba_adapter_free(a);

	ck_assert_ptr_ne(a = ba_adapter_new(5, "test"), NULL);
	ck_assert_int_eq(a->hci_dev_id, 5);
	ck_assert_str_eq(a->hci_name, "test");
	ba_adapter_free(a);

} END_TEST

START_TEST(test_ba_device) {

	struct ba_adapter *a;
	struct ba_device *d;

	ck_assert_ptr_ne(a = ba_adapter_new(0, NULL), NULL);

	bdaddr_t addr = {{ 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB }};
	ck_assert_ptr_ne(d = ba_device_new(a, &addr, "Test Device"), NULL);

	ck_assert_ptr_eq(d->a, a);
	ck_assert_int_eq(bacmp(&d->addr, &addr), 0);
	ck_assert_str_eq(d->name, "Test Device");

	ba_device_free(d);
	ba_adapter_free(a);

} END_TEST

START_TEST(test_ba_transport) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0, NULL), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr, "Test"), NULL);

	struct ba_transport_type type = { 0 };
	ck_assert_ptr_ne(t = transport_new(d, type, "/owner", "/path"), NULL);

	ck_assert_ptr_eq(t->d, d);
	ck_assert_int_eq(memcmp(&t->type, &type, sizeof(type)), 0);
	ck_assert_str_eq(t->dbus_owner, "/owner");
	ck_assert_str_eq(t->dbus_path, "/path");

	ba_transport_free(t);
	ba_device_free(d);
	ba_adapter_free(a);

} END_TEST

START_TEST(test_cascade_free) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	struct ba_transport_type type = { 0 };
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0, NULL), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr, "Test"), NULL);
	ck_assert_ptr_ne(t = transport_new(d, type, "/owner", "/path"), NULL);

	ba_adapter_free(a);

} END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_ba_adapter);
	tcase_add_test(tc, test_ba_device);
	tcase_add_test(tc, test_ba_transport);
	tcase_add_test(tc, test_cascade_free);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
