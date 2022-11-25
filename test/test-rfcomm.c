/*
 * test-rfcomm.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
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

#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluez.h"
#include "hfp.h"
#include "shared/log.h"

static struct ba_adapter *adapter = NULL;
static struct ba_device *device1 = NULL;
static struct ba_device *device2 = NULL;

static pthread_mutex_t transport_codec_updated_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t transport_codec_updated = PTHREAD_COND_INITIALIZER;
static unsigned int transport_codec_updated_cnt = 0;

void a2dp_aac_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_aac_transport_start(struct ba_transport *t) { (void)t; return 0; }
void a2dp_aptx_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_aptx_transport_start(struct ba_transport *t) { (void)t; return 0; }
void a2dp_aptx_hd_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_aptx_hd_transport_start(struct ba_transport *t) { (void)t; return 0; }
void a2dp_faststream_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_faststream_transport_start(struct ba_transport *t) { (void)t; return 0; }
void a2dp_lc3plus_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_lc3plus_transport_start(struct ba_transport *t) { (void)t; return 0; }
void a2dp_ldac_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_ldac_transport_start(struct ba_transport *t) { (void)t; return 0; }
void *sco_enc_thread(struct ba_transport_thread *th) { return sleep(3600), th; }
void *sco_dec_thread(struct ba_transport_thread *th) { return sleep(3600), th; }
void a2dp_mpeg_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_mpeg_transport_start(struct ba_transport *t) { (void)t; return 0; }
void a2dp_sbc_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_sbc_transport_start(struct ba_transport *t) { (void)t; return 0; }
int storage_device_load(const struct ba_device *d) { (void)d; return 0; }
int storage_device_save(const struct ba_device *d) { (void)d; return 0; }
int storage_pcm_data_sync(struct ba_transport_pcm *pcm) { (void)pcm; return 0; }
int storage_pcm_data_update(const struct ba_transport_pcm *pcm) { (void)pcm; return 0; }

int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm);
	pcm->ba_dbus_exported = true;
	return 0; }
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)pcm, mask); (void)pcm;
	if (mask & BA_DBUS_PCM_UPDATE_CODEC) {
		pthread_mutex_lock(&transport_codec_updated_mtx);
		transport_codec_updated_cnt++;
		pthread_mutex_unlock(&transport_codec_updated_mtx);
		pthread_cond_signal(&transport_codec_updated); }}
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); (void)pcm; }
int bluealsa_dbus_rfcomm_register(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); (void)r; return 0; }
void bluealsa_dbus_rfcomm_update(struct ba_rfcomm *r, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)r, mask); (void)r; (void)mask; }
void bluealsa_dbus_rfcomm_unregister(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); (void)r; }
bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s: %p", __func__, current_dbus_sep_path, sep);
	(void)current_dbus_sep_path; (void)sep; (void)error; return false; }
void bluez_battery_provider_update(struct ba_device *device) {
	debug("%s: %p", __func__, device); (void)device; }

static uint16_t get_codec_id(struct ba_transport *t) {
	pthread_mutex_lock(&t->codec_id_mtx);
	uint16_t codec_id = t->codec_id;
	pthread_mutex_unlock(&t->codec_id_mtx);
	return codec_id;
}

START_TEST(test_rfcomm) {

	transport_codec_updated_cnt = 0;
	memset(adapter->hci.features, 0, sizeof(adapter->hci.features));

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	struct ba_transport *ag = ba_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HFP_AG, ":test", "/sco/ag", fds[0]);
	struct ba_transport *hf = ba_transport_new_sco(device2,
			BA_TRANSPORT_PROFILE_HFP_HF, ":test", "/sco/hf", fds[1]);

	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_CVSD);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_CVSD);

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection (SLC established) signals */
	while (transport_codec_updated_cnt < 0 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	ck_assert_int_eq(device1->ref_count, 1 + 1);
	ck_assert_int_eq(device2->ref_count, 1 + 1);

	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_CVSD);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_CVSD);

	debug("Audio gateway destroying");
	ba_transport_destroy(ag);
	/* The hf transport shall be destroyed by the "link lost" quirk. However,
	 * we have to wait "some" time before reference counter check, because
	 * this action is asynchronous from our point of view. */
	debug("Hands Free unreferencing");
	ba_transport_unref(hf);
	debug("Wait for asynchronous free");
	usleep(100000);

	ck_assert_int_eq(device1->ref_count, 1);
	ck_assert_int_eq(device2->ref_count, 1);

} END_TEST

START_TEST(test_rfcomm_esco) {

	transport_codec_updated_cnt = 0;
	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	struct ba_transport *ag = ba_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HFP_AG, ":test", "/sco/ag", fds[0]);
	struct ba_transport *hf = ba_transport_new_sco(device2,
			BA_TRANSPORT_PROFILE_HFP_HF, ":test", "/sco/hf", fds[1]);

	ag->sco.rfcomm->link_lost_quirk = false;
	hf->sco.rfcomm->link_lost_quirk = false;

#if ENABLE_MSBC

	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_UNDEFINED);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_UNDEFINED);

	/* wait for SLC established */
	while (ag->sco.rfcomm->state != HFP_SLC_CONNECTED)
		usleep(10000);
	while (hf->sco.rfcomm->state != HFP_SLC_CONNECTED)
		usleep(10000);

#else

	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_CVSD);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_CVSD);

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection (SLC establishment) signals */
	while (transport_codec_updated_cnt < 0 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

#endif

	ck_assert_int_eq(device1->ref_count, 1 + 1);
	ck_assert_int_eq(device2->ref_count, 1 + 1);

#if ENABLE_MSBC
	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 0 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);
#endif

#if ENABLE_MSBC
	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_MSBC);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_MSBC);
#else
	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_CVSD);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_CVSD);
#endif

	debug("Audio gateway destroying");
	ba_transport_destroy(ag);
	debug("Hands Free destroying");
	ba_transport_destroy(hf);

	ck_assert_int_eq(device1->ref_count, 1);
	ck_assert_int_eq(device2->ref_count, 1);

} END_TEST

#if ENABLE_MSBC
START_TEST(test_rfcomm_set_codec) {

	transport_codec_updated_cnt = 0;
	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	struct ba_transport *ag = ba_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HFP_AG, ":test", "/sco/ag", fds[0]);
	struct ba_transport *hf = ba_transport_new_sco(device2,
			BA_TRANSPORT_PROFILE_HFP_HF, ":test", "/sco/hf", fds[1]);

	ag->sco.rfcomm->link_lost_quirk = false;
	hf->sco.rfcomm->link_lost_quirk = false;

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 0 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	/* Allow RFCOMM thread to finalize internal codec selection. This sleep is
	 * required, because we are waiting on "codec-updated" signal which is sent
	 * by the RFCOMM thread before the codec selection completeness signal. And
	 * this latter signal might prematurely wake ba_transport_select_codec_sco()
	 * function. */
	usleep(10000);

	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_MSBC);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_MSBC);

	/* select different audio codec */
	ck_assert_int_eq(ba_transport_select_codec_sco(ag, HFP_CODEC_CVSD), 0);

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 4 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_CVSD);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_CVSD);

	/* select already selected audio codec */
	ck_assert_int_eq(ba_transport_select_codec_sco(ag, HFP_CODEC_CVSD), 0);

	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_CVSD);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_CVSD);

	/* switch back audio codec */
	ck_assert_int_eq(ba_transport_select_codec_sco(ag, HFP_CODEC_MSBC), 0);

	pthread_mutex_lock(&transport_codec_updated_mtx);
	/* wait for codec selection signals */
	while (transport_codec_updated_cnt < 8 + (2 + 2))
		pthread_cond_wait(&transport_codec_updated, &transport_codec_updated_mtx);
	pthread_mutex_unlock(&transport_codec_updated_mtx);

	ck_assert_int_eq(get_codec_id(ag), HFP_CODEC_MSBC);
	ck_assert_int_eq(get_codec_id(hf), HFP_CODEC_MSBC);

} END_TEST
#endif

int main(void) {

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	adapter = ba_adapter_new(0);
	bdaddr_t addr1 = {{ 1, 1, 1, 1, 1, 1 }};
	device1 = ba_device_new(adapter, &addr1);
	bdaddr_t addr2 = {{ 2, 2, 2, 2, 2, 2 }};
	device2 = ba_device_new(adapter, &addr2);

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
