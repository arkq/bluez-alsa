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
#include "../src/bluealsa-dbus.c"
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
	pthread_mutex_lock(&a->devices_mutex);

	bdaddr_t addr = {{ 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB }};
	ck_assert_ptr_ne(d = ba_device_new(a, &addr, "Test Device"), NULL);

	ck_assert_ptr_eq(d->a, a);
	ck_assert_int_eq(bacmp(&d->addr, &addr), 0);
	ck_assert_str_eq(d->ba_dbus_path, "/org/bluealsa/hci0/dev_AB_90_78_56_34_12");
	ck_assert_str_eq(d->bluez_dbus_path, "/org/bluez/hci0/dev_AB_90_78_56_34_12");
	ck_assert_str_eq(d->name, "Test Device");

	ba_device_free(d);
	pthread_mutex_unlock(&a->devices_mutex);
	ba_adapter_free(a);

} END_TEST

START_TEST(test_ba_transport) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0, NULL), NULL);
	pthread_mutex_lock(&a->devices_mutex);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr, "Test"), NULL);

	struct ba_transport_type type = { 0 };
	ck_assert_ptr_ne(t = ba_transport_new(d, type, "/owner", "/path"), NULL);

	ck_assert_ptr_eq(t->d, d);
	ck_assert_int_eq(memcmp(&t->type, &type, sizeof(type)), 0);
	ck_assert_str_eq(t->bluez_dbus_owner, "/owner");
	ck_assert_str_eq(t->bluez_dbus_path, "/path");

	ba_transport_free(t);
	ba_device_free(d);
	pthread_mutex_unlock(&a->devices_mutex);
	ba_adapter_free(a);

} END_TEST

START_TEST(test_ba_transport_volume_packed) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	bdaddr_t addr = { 0 };
	struct ba_transport_type type = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0, NULL), NULL);
	pthread_mutex_lock(&a->devices_mutex);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr, "Test"), NULL);
	ck_assert_ptr_ne(t = ba_transport_new(d, type, "/owner", "/path"), NULL);

	t->a2dp.ch1_muted = true;
	t->a2dp.ch1_volume = 0x6C;
	t->a2dp.ch2_muted = false;
	t->a2dp.ch2_volume = 0x4F;
	t->type.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	ck_assert_uint_eq(ba_transport_get_volume_packed(t), 0xEC4F);

	ck_assert_int_eq(ba_transport_set_volume_packed(t, 0xB0C1), 0);
	ck_assert_int_eq(!!t->a2dp.ch1_muted, true);
	ck_assert_int_eq(t->a2dp.ch1_volume, 48);
	ck_assert_int_eq(!!t->a2dp.ch2_muted, true);
	ck_assert_int_eq(t->a2dp.ch2_volume, 65);

	t->sco.spk_muted = false;
	t->sco.spk_gain = 0x0A;
	t->sco.mic_muted = true;
	t->sco.mic_gain = 0x05;
	t->type.profile = BA_TRANSPORT_PROFILE_HFP_AG;
	ck_assert_uint_eq(ba_transport_get_volume_packed(t), 0x0A85);

	ck_assert_int_eq(ba_transport_set_volume_packed(t, 0x8A0B), 0);
	ck_assert_int_eq(!!t->sco.spk_muted, true);
	ck_assert_int_eq(t->sco.spk_gain, 10);
	ck_assert_int_eq(!!t->sco.mic_muted, false);
	ck_assert_int_eq(t->sco.mic_gain, 11);

	t->type.profile = 0;
	ba_transport_free(t);
	ba_device_free(d);
	pthread_mutex_unlock(&a->devices_mutex);
	ba_adapter_free(a);

} END_TEST

START_TEST(test_cascade_free) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	struct ba_transport_type type = { 0 };
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0, NULL), NULL);
	pthread_mutex_lock(&a->devices_mutex);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr, "Test"), NULL);
	ck_assert_ptr_ne(t = ba_transport_new(d, type, "/owner", "/path"), NULL);
	pthread_mutex_unlock(&a->devices_mutex);

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
	tcase_add_test(tc, test_ba_transport_volume_packed);
	tcase_add_test(tc, test_cascade_free);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
