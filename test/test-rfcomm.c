/*
 * test-rfcomm.c
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
#include "../src/at.c"
#include "../src/hci.c"
#include "../src/rfcomm.c"
#include "../src/utils.c"
#include "../src/shared/log.c"

int bluealsa_dbus_transport_register(struct ba_transport *t, GError **error) {
	debug("%s: %p", __func__, (void *)t); (void)error;
	return 0; }
void bluealsa_dbus_transport_update(struct ba_transport *t, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)t, mask); }
void bluealsa_dbus_transport_unregister(struct ba_transport *t) {
	debug("%s: %p", __func__, (void *)t); }
int a2dp_thread_create(struct ba_transport *t) { (void)t; return -1; }
void *sco_thread(struct ba_transport *t) { return sleep(3600), t; }

static struct ba_adapter *adapter = NULL;
static struct ba_device *device = NULL;

START_TEST(test_rfcomm) {

	memset(adapter->hci.features, 0, sizeof(adapter->hci.features));

	struct ba_transport_type ttype_ag = { .profile = BA_TRANSPORT_PROFILE_HFP_AG };
	struct ba_transport *ag = ba_transport_new_rfcomm(device, ttype_ag, ":test", "/rfcomm/ag");
	struct ba_transport_type ttype_hf = { .profile = BA_TRANSPORT_PROFILE_HFP_HF };
	struct ba_transport *hf = ba_transport_new_rfcomm(device, ttype_hf, ":test", "/rfcomm/hf");

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	ag->bt_fd = fds[0];
	hf->bt_fd = fds[1];

	ck_assert_int_eq(ag->rfcomm.sco->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->rfcomm.sco->type.codec, HFP_CODEC_CVSD);

	ba_transport_set_state(ag, TRANSPORT_ACTIVE);
	ba_transport_set_state(hf, TRANSPORT_ACTIVE);

	poll(NULL, 0, 1000 + RFCOMM_TIMEOUT_IDLE);
	ck_assert_int_eq(device->ref_count, 1 + 4);

	ck_assert_int_eq(ag->rfcomm.sco->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->rfcomm.sco->type.codec, HFP_CODEC_CVSD);

	ba_transport_destroy(ag);
	ba_transport_destroy(hf);
	ck_assert_int_eq(device->ref_count, 1);

} END_TEST

START_TEST(test_rfcomm_esco) {

	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;

	struct ba_transport_type ttype_ag = { .profile = BA_TRANSPORT_PROFILE_HFP_AG };
	struct ba_transport *ag = ba_transport_new_rfcomm(device, ttype_ag, ":test", "/rfcomm/ag");
	struct ba_transport_type ttype_hf = { .profile = BA_TRANSPORT_PROFILE_HFP_HF };
	struct ba_transport *hf = ba_transport_new_rfcomm(device, ttype_hf, ":test", "/rfcomm/hf");

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	ag->bt_fd = fds[0];
	hf->bt_fd = fds[1];

#if ENABLE_MSBC
	ck_assert_int_eq(ag->rfcomm.sco->type.codec, HFP_CODEC_UNDEFINED);
	ck_assert_int_eq(hf->rfcomm.sco->type.codec, HFP_CODEC_UNDEFINED);
#else
	ck_assert_int_eq(ag->rfcomm.sco->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->rfcomm.sco->type.codec, HFP_CODEC_CVSD);
#endif

	ba_transport_set_state(ag, TRANSPORT_ACTIVE);
	ba_transport_set_state(hf, TRANSPORT_ACTIVE);

	poll(NULL, 0, 1000 + RFCOMM_TIMEOUT_IDLE);
	ck_assert_int_eq(device->ref_count, 1 + 4);

#if ENABLE_MSBC
	ck_assert_int_eq(ag->rfcomm.sco->type.codec, HFP_CODEC_MSBC);
	ck_assert_int_eq(hf->rfcomm.sco->type.codec, HFP_CODEC_MSBC);
#else
	ck_assert_int_eq(ag->rfcomm.sco->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->rfcomm.sco->type.codec, HFP_CODEC_CVSD);
#endif

	ba_transport_destroy(ag);
	ba_transport_destroy(hf);
	ck_assert_int_eq(device->ref_count, 1);

} END_TEST


int main(void) {

	bdaddr_t addr = {{ 1, 2, 3, 4, 5, 6 }};
	adapter = ba_adapter_new(0);
	device = ba_device_new(adapter, &addr);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);
	tcase_set_timeout(tc, 6);

	tcase_add_test(tc, test_rfcomm);
	tcase_add_test(tc, test_rfcomm_esco);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
