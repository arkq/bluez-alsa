/*
 * test-rfcomm.c
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

#include <pthread.h>
#include <signal.h>

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

static struct ba_adapter *adapter = NULL;
static struct ba_device *device = NULL;

static pthread_mutex_t transport_codec_updated_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t transport_codec_updated = PTHREAD_COND_INITIALIZER;
static unsigned int transport_codec_updated_cnt = 0;

unsigned int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm, GError **error) {
	debug("%s: %p", __func__, (void *)pcm); (void)error;
	return 0; }
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)pcm, mask);
	if (mask & BA_DBUS_PCM_UPDATE_CODEC) {
		pthread_mutex_lock(&transport_codec_updated_mtx);
		transport_codec_updated_cnt++;
		pthread_cond_signal(&transport_codec_updated);
		pthread_mutex_unlock(&transport_codec_updated_mtx); }}
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); }
unsigned int bluealsa_dbus_rfcomm_register(struct ba_transport *t, GError **error) {
	debug("%s: %p", __func__, (void *)t); (void)error; return 0; }
void bluealsa_dbus_rfcomm_update(struct ba_transport *t, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)t, mask); }
void bluealsa_dbus_rfcomm_unregister(struct ba_transport *t) {
	debug("%s: %p", __func__, (void *)t); }
int a2dp_thread_create(struct ba_transport *t) { (void)t; return -1; }
void *sco_thread(struct ba_transport *t) { return sleep(3600), t; }

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

	pthread_mutex_lock(&transport_codec_updated_mtx);
	transport_codec_updated_cnt = 0;

	ck_assert_int_eq(ba_transport_set_state(ag, BA_TRANSPORT_STATE_ACTIVE), 0);
	ck_assert_int_eq(ba_transport_set_state(hf, BA_TRANSPORT_STATE_ACTIVE), 0);

	/* wait for SLC established signals */
	while (transport_codec_updated_cnt < 2)
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	while (transport_codec_updated_cnt < 4)
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);

	ck_assert_int_eq(device->ref_count, 1 + 4);

	ck_assert_int_eq(ag->rfcomm.sco->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->rfcomm.sco->type.codec, HFP_CODEC_CVSD);

	ba_transport_destroy(ag);
	ba_transport_destroy(hf);
	ck_assert_int_eq(device->ref_count, 1);

	pthread_mutex_unlock(&transport_codec_updated_mtx);

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

	pthread_mutex_lock(&transport_codec_updated_mtx);
	transport_codec_updated_cnt = 0;

	ck_assert_int_eq(ba_transport_set_state(ag, BA_TRANSPORT_STATE_ACTIVE), 0);
	ck_assert_int_eq(ba_transport_set_state(hf, BA_TRANSPORT_STATE_ACTIVE), 0);

	/* wait for SLC established signals */
	while (transport_codec_updated_cnt < 2)
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	while (transport_codec_updated_cnt < 4)
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);

	ck_assert_int_eq(device->ref_count, 1 + 4);

#if ENABLE_MSBC
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 6)
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	while (transport_codec_updated_cnt < 8)
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
#endif

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

	pthread_mutex_unlock(&transport_codec_updated_mtx);

} END_TEST

int main(void) {

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	bdaddr_t addr = {{ 1, 2, 3, 4, 5, 6 }};
	adapter = ba_adapter_new(0);
	device = ba_device_new(adapter, &addr);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);
	tcase_set_timeout(tc, 6);

	config.battery.available = true;
	config.battery.level = 80;

	tcase_add_test(tc, test_rfcomm);
	tcase_add_test(tc, test_rfcomm_esco);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
