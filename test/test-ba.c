/*
 * test-ba.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
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
#include "../src/hci.c"
#include "../src/utils.c"
#include "../src/shared/log.c"

int a2dp_thread_create(struct ba_transport *t) { (void)t; return 0; }
void *rfcomm_thread(struct ba_transport *t) { (void)t; return 0; }
void *sco_thread(struct ba_transport *t) { (void)t; return 0; }
unsigned int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm, GError **error) {
	debug("%s: %p", __func__, (void *)pcm); (void)error; return 0; }
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)pcm, mask); }
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); }
unsigned int bluealsa_dbus_rfcomm_register(struct ba_transport *t, GError **error) {
	debug("%s: %p", __func__, (void *)t); (void)error; return 0; }
void bluealsa_dbus_rfcomm_update(struct ba_transport *t, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)t, mask); }
void bluealsa_dbus_rfcomm_unregister(struct ba_transport *t) {
	debug("%s: %p", __func__, (void *)t); }

START_TEST(test_ba_adapter) {

	struct ba_adapter *a;

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_str_eq(a->hci.name, "hci0");
	ba_adapter_unref(a);

	ck_assert_ptr_ne(a = ba_adapter_new(5), NULL);
	ck_assert_int_eq(a->hci.dev_id, 5);
	ck_assert_str_eq(a->hci.name, "hci5");
	ba_adapter_unref(a);

} END_TEST

START_TEST(test_ba_device) {

	struct ba_adapter *a;
	struct ba_device *d;

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);

	bdaddr_t addr = {{ 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB }};
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	ba_adapter_unref(a);

	ck_assert_ptr_eq(d->a, a);
	ck_assert_int_eq(bacmp(&d->addr, &addr), 0);
	ck_assert_str_eq(d->ba_dbus_path, "/org/bluealsa/hci0/dev_AB_90_78_56_34_12");
	ck_assert_str_eq(d->bluez_dbus_path, "/org/bluez/hci0/dev_AB_90_78_56_34_12");

	ba_device_unref(d);

} END_TEST

START_TEST(test_ba_transport) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	ck_assert_ptr_ne(t = ba_transport_new(d, "/owner", "/path"), NULL);

	ba_adapter_unref(a);
	ba_device_unref(d);

	ck_assert_ptr_eq(t->d, d);
	ck_assert_int_eq(t->type.profile, BA_TRANSPORT_PROFILE_NONE);
	ck_assert_str_eq(t->bluez_dbus_owner, "/owner");
	ck_assert_str_eq(t->bluez_dbus_path, "/path");

	ba_transport_unref(t);

} END_TEST

START_TEST(test_ba_transport_volume_packed) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t_a2dp;
	struct ba_transport *t_sco;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	struct ba_transport_type ttype_a2dp = { .profile = BA_TRANSPORT_PROFILE_A2DP_SINK };
	a2dp_sbc_t cconfig = { .channel_mode = SBC_CHANNEL_MODE_STEREO };
	ck_assert_ptr_ne(t_a2dp = ba_transport_new_a2dp(d, ttype_a2dp,
				"/owner", "/path", &cconfig, sizeof(cconfig)), NULL);

	struct ba_transport_type ttype_sco = { .profile = BA_TRANSPORT_PROFILE_HFP_AG };
	ck_assert_ptr_ne(t_sco = ba_transport_new_sco(d, ttype_sco, "/owner", "/path", NULL), NULL);

	ba_adapter_unref(a);
	ba_device_unref(d);

	t_a2dp->a2dp.pcm.volume[0].muted = true;
	t_a2dp->a2dp.pcm.volume[0].level = 0x6C;
	t_a2dp->a2dp.pcm.volume[1].muted = false;
	t_a2dp->a2dp.pcm.volume[1].level = 0x4F;
	ck_assert_uint_eq(ba_transport_pcm_get_volume_packed(&t_a2dp->a2dp.pcm), 0xEC4F);

	ck_assert_int_eq(ba_transport_pcm_set_volume_packed(&t_a2dp->a2dp.pcm, 0xB0C1), 0);
	ck_assert_int_eq(!!t_a2dp->a2dp.pcm.volume[0].muted, true);
	ck_assert_int_eq(t_a2dp->a2dp.pcm.volume[0].level, 48);
	ck_assert_int_eq(!!t_a2dp->a2dp.pcm.volume[1].muted, true);
	ck_assert_int_eq(t_a2dp->a2dp.pcm.volume[1].level, 65);

	t_sco->sco.spk_pcm.volume[0].muted = false;
	t_sco->sco.spk_pcm.volume[0].level = 0x0A;
	t_sco->sco.mic_pcm.volume[0].muted = true;
	t_sco->sco.mic_pcm.volume[0].level = 0x05;
	ck_assert_uint_eq(ba_transport_pcm_get_volume_packed(&t_sco->sco.spk_pcm), 0x0A00);
	ck_assert_uint_eq(ba_transport_pcm_get_volume_packed(&t_sco->sco.mic_pcm), 0x8500);

	ck_assert_int_eq(ba_transport_pcm_set_volume_packed(&t_sco->sco.spk_pcm, 0x8A00), 0);
	ck_assert_int_eq(ba_transport_pcm_set_volume_packed(&t_sco->sco.mic_pcm, 0x0B00), 0);
	ck_assert_int_eq(!!t_sco->sco.spk_pcm.volume[0].muted, true);
	ck_assert_int_eq(t_sco->sco.spk_pcm.volume[0].level, 10);
	ck_assert_int_eq(!!t_sco->sco.mic_pcm.volume[0].muted, false);
	ck_assert_int_eq(t_sco->sco.mic_pcm.volume[0].level, 11);

	ba_transport_unref(t_a2dp);
	ba_transport_unref(t_sco);

} END_TEST

static int test_cascade_free_transport_unref(struct ba_transport *t) {
	return ba_transport_unref(t), 0;
}

START_TEST(test_cascade_free) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);
	ck_assert_ptr_ne(t = ba_transport_new(d, "/owner", "/path"), NULL);
	t->release = test_cascade_free_transport_unref;

	ba_device_unref(d);
	ba_adapter_destroy(a);

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
