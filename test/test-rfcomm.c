/*
 * test-rfcomm.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
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
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <check.h>
#include <glib.h>

#include "a2dp.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "bluealsa-dbus.h"
#include "bluealsa.h"
#include "bluez.h"
#include "hfp.h"
#include "shared/log.h"

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
unsigned int bluealsa_dbus_rfcomm_register(struct ba_rfcomm *r, GError **error) {
	debug("%s: %p", __func__, (void *)r); (void)error; return 0; }
void bluealsa_dbus_rfcomm_update(struct ba_rfcomm *r, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)r, mask); }
void bluealsa_dbus_rfcomm_unregister(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); }
int a2dp_audio_thread_create(struct ba_transport *t) { (void)t; return -1; }
void *sco_enc_thread(struct ba_transport_thread *th) { return sleep(3600), th; }
void *sco_dec_thread(struct ba_transport_thread *th) { return sleep(3600), th; }
bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s", __func__, current_dbus_sep_path); (void)sep;
	(void)error; return false; }

START_TEST(test_rfcomm) {

	transport_codec_updated_cnt = 0;
	memset(adapter->hci.features, 0, sizeof(adapter->hci.features));

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	struct ba_transport_type ttype_ag = { .profile = BA_TRANSPORT_PROFILE_HFP_AG };
	struct ba_transport *ag = ba_transport_new_sco(device, ttype_ag, ":test", "/sco/ag", fds[0]);
	struct ba_transport_type ttype_hf = { .profile = BA_TRANSPORT_PROFILE_HFP_HF };
	struct ba_transport *hf = ba_transport_new_sco(device, ttype_hf, ":test", "/sco/hf", fds[1]);

	ck_assert_int_eq(ag->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_CVSD);

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for SLC established signals */
	while (transport_codec_updated_cnt < 0 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	ck_assert_int_eq(device->ref_count, 1 + 2);

	ck_assert_int_eq(ag->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_CVSD);

	debug("Audio gateway destroying");
	ba_transport_destroy(ag);
	/* The hf transport shall be destroyed by the "link lost" quirk. However,
	 * we have to wait "some" time before reference counter check, because
	 * this action is asynchronous from our point of view. */
	debug("Hands Free unreferencing");
	ba_transport_unref(hf);
	debug("Wait for asynchronous free");
	usleep(100000);

	ck_assert_int_eq(device->ref_count, 1);

} END_TEST

START_TEST(test_rfcomm_esco) {

	transport_codec_updated_cnt = 0;
	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	struct ba_transport_type ttype_ag = { .profile = BA_TRANSPORT_PROFILE_HFP_AG };
	struct ba_transport *ag = ba_transport_new_sco(device, ttype_ag, ":test", "/sco/ag", fds[0]);
	struct ba_transport_type ttype_hf = { .profile = BA_TRANSPORT_PROFILE_HFP_HF };
	struct ba_transport *hf = ba_transport_new_sco(device, ttype_hf, ":test", "/sco/hf", fds[1]);

	ag->sco.rfcomm->link_lost_quirk = false;
	hf->sco.rfcomm->link_lost_quirk = false;

#if ENABLE_MSBC
	ck_assert_int_eq(ag->type.codec, HFP_CODEC_UNDEFINED);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_UNDEFINED);
#else
	ck_assert_int_eq(ag->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_CVSD);
#endif

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for SLC established signals */
	while (transport_codec_updated_cnt < 0 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	ck_assert_int_eq(device->ref_count, 1 + 2);

#if ENABLE_MSBC
	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 4 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);
#endif

#if ENABLE_MSBC
	ck_assert_int_eq(ag->type.codec, HFP_CODEC_MSBC);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_MSBC);
#else
	ck_assert_int_eq(ag->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_CVSD);
#endif

	debug("Audio gateway destroying");
	ba_transport_destroy(ag);
	debug("Hands Free destroying");
	ba_transport_destroy(hf);

	ck_assert_int_eq(device->ref_count, 1);

} END_TEST

#if ENABLE_MSBC
START_TEST(test_rfcomm_set_codec) {

	transport_codec_updated_cnt = 0;
	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	struct ba_transport_type ttype_ag = { .profile = BA_TRANSPORT_PROFILE_HFP_AG };
	struct ba_transport *ag = ba_transport_new_sco(device, ttype_ag, ":test", "/sco/ag", fds[0]);
	struct ba_transport_type ttype_hf = { .profile = BA_TRANSPORT_PROFILE_HFP_HF };
	struct ba_transport *hf = ba_transport_new_sco(device, ttype_hf, ":test", "/sco/hf", fds[1]);

	ag->sco.rfcomm->link_lost_quirk = false;
	hf->sco.rfcomm->link_lost_quirk = false;

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for SLC established signals */
	while (transport_codec_updated_cnt < 0 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 4 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	/* Allow RFCOMM thread to finalize internal codec selection. This sleep is
	 * required, because we are waiting on "codec-updated" signal which is sent
	 * by the RFCOMM thread before the codec selection completeness signal. And
	 * this latter signal might prematurely wake ba_transport_select_codec_sco()
	 * function. */
	usleep(10000);

	ck_assert_int_eq(ag->type.codec, HFP_CODEC_MSBC);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_MSBC);

	/* select different audio codec */
	ck_assert_int_eq(ba_transport_select_codec_sco(ag, HFP_CODEC_CVSD), 0);

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 8 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	ck_assert_int_eq(ag->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_CVSD);

	/* select already selected audio codec */
	ck_assert_int_eq(ba_transport_select_codec_sco(ag, HFP_CODEC_CVSD), 0);

	ck_assert_int_eq(ag->type.codec, HFP_CODEC_CVSD);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_CVSD);

	/* switch back audio codec */
	ck_assert_int_eq(ba_transport_select_codec_sco(ag, HFP_CODEC_MSBC), 0);

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 12 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	ck_assert_int_eq(ag->type.codec, HFP_CODEC_MSBC);
	ck_assert_int_eq(hf->type.codec, HFP_CODEC_MSBC);

} END_TEST
#endif

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
	tcase_set_timeout(tc, 10);

	config.battery.available = true;
	config.battery.level = 80;

	tcase_add_test(tc, test_rfcomm);
	tcase_add_test(tc, test_rfcomm_esco);
#if ENABLE_MSBC
	tcase_add_test(tc, test_rfcomm_set_codec);
#endif

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
