/*
 * test-rfcomm.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <check.h>
#include <glib.h>

#include "a2dp.h"
#include "ba-adapter.h"
#include "ba-config.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "ble-midi.h"
#include "bluealsa-dbus.h"
#include "bluez.h"
#include "hfp.h"
#include "shared/log.h"

#include "inc/check.inc"

static struct ba_adapter *adapter = NULL;
static struct ba_device *device1 = NULL;
static struct ba_device *device2 = NULL;

static pthread_mutex_t dbus_update_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dbus_update_cond = PTHREAD_COND_INITIALIZER;
static struct {
	unsigned int codec;
	unsigned int volume;
	unsigned int battery;
} dbus_update_counters;

static void dbus_update_counters_wait(unsigned int *counter, unsigned int value) {
	pthread_mutex_lock(&dbus_update_mtx);
	while (*counter != value)
		pthread_cond_wait(&dbus_update_cond, &dbus_update_mtx);
	pthread_mutex_unlock(&dbus_update_mtx);
}

void ble_midi_decode_free(struct ble_midi_dec *bmd) { (void)bmd; }
int midi_transport_alsa_seq_create(struct ba_transport *t) { (void)t; return 0; }
int midi_transport_alsa_seq_delete(struct ba_transport *t) { (void)t; return 0; }
int midi_transport_start(struct ba_transport *t) { (void)t; return 0; }
int midi_transport_stop(struct ba_transport *t) { (void)t; return 0; }
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
	pthread_mutex_lock(&dbus_update_mtx);
	dbus_update_counters.codec += !!(mask & BA_DBUS_PCM_UPDATE_CODEC);
	dbus_update_counters.volume += !!(mask & BA_DBUS_PCM_UPDATE_VOLUME);
	pthread_mutex_unlock(&dbus_update_mtx);
	pthread_cond_signal(&dbus_update_cond); }
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); (void)pcm; }
int bluealsa_dbus_rfcomm_register(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); (void)r; return 0; }
void bluealsa_dbus_rfcomm_update(struct ba_rfcomm *r, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)r, mask); (void)r;
	pthread_mutex_lock(&dbus_update_mtx);
	dbus_update_counters.battery += !!(mask & BA_DBUS_RFCOMM_UPDATE_BATTERY);
	pthread_mutex_unlock(&dbus_update_mtx);
	pthread_cond_signal(&dbus_update_cond); }
void bluealsa_dbus_rfcomm_unregister(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); (void)r; }
bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep_config *sep, const void *configuration, GError **error) {
	debug("%s: %s: %p", __func__, current_dbus_sep_path, sep);
	(void)current_dbus_sep_path; (void)sep; (void)configuration; (void)error;
	return false; }
void bluez_battery_provider_update(struct ba_device *device) {
	debug("%s: %p", __func__, device); (void)device; }
int ofono_call_volume_update(struct ba_transport *t) {
	debug("%s: %p", __func__, t); (void)t; return 0; }

#define ck_assert_rfcomm_recv(fd, command) { \
	char buffer[sizeof(command)] = { 0 }; \
	const ssize_t len = strlen(command); \
	ck_assert_int_eq(read(fd, buffer, sizeof(buffer) - 1), len); \
	ck_assert_str_eq(buffer, command); }

#define ck_assert_rfcomm_send(fd, command) { \
	const ssize_t len = strlen(command); \
	ck_assert_int_eq(write(fd, command, len), len); }

CK_START_TEST(test_rfcomm_hsp_ag) {

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
	struct ba_transport *sco = ba_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HSP_AG, ":test", "/sco", fds[0]);
	const int fd = fds[1];

	/* check support for setting microphone gain */
	ck_assert_rfcomm_send(fd, "AT+VGM=10\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.volume, 1);

	/* check support for setting speaker gain */
	ck_assert_rfcomm_send(fd, "AT+VGS=13\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.volume, 2);

	/* check support for button press */
	ck_assert_rfcomm_send(fd, "AT+CKPD=200\r");
	ck_assert_rfcomm_recv(fd, "\r\nERROR\r\n");

	/* check vendor-specific command for battery level notification */
	ck_assert_rfcomm_send(fd, "AT+IPHONEACCEV=2,1,8,2,1\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.battery, 1);

	ba_transport_destroy(sco);
	close(fd);

} CK_END_TEST

CK_START_TEST(test_rfcomm_hsp_hs) {

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
	struct ba_transport *sco = ba_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HSP_HS, ":test", "/sco", fds[0]);
	const int fd = fds[1];

	/* wait for initial microphone gain */
	ck_assert_rfcomm_recv(fd, "AT+VGM=15\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

	/* wait for initial speaker gain */
	ck_assert_rfcomm_recv(fd, "AT+VGS=15\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

	/* wait for initial vendor-specific battery level */
	ck_assert_rfcomm_recv(fd, "AT+XAPL=DEAD-C0DE-1234,6\r");
	ck_assert_rfcomm_send(fd, "\r\n+XAPL:TEST,6\r\n");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");
	ck_assert_rfcomm_recv(fd, "AT+IPHONEACCEV=2,1,8,2,0\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

	/* check support for setting speaker gain */
	ck_assert_rfcomm_send(fd, "\r\n+VGS=13\r\n");
	dbus_update_counters_wait(&dbus_update_counters.volume, 1);

	/* check support for setting microphone gain */
	ck_assert_rfcomm_send(fd, "\r\n+VGM=10\r\n");
	dbus_update_counters_wait(&dbus_update_counters.volume, 2);

	/* check support for RING command */
	ck_assert_rfcomm_send(fd, "\r\nRING\r\n");
	usleep(5000);

	ba_transport_destroy(sco);
	close(fd);

} CK_END_TEST

#if ENABLE_HFP_CODEC_SELECTION
static void *test_rfcomm_hfp_ag_switch_codecs(void *userdata) {
	struct ba_transport *sco = userdata;
	pthread_mutex_lock(&sco->codec_select_client_mtx);
	ck_assert_int_eq(ba_transport_select_codec_sco(sco, HFP_CODEC_CVSD), 0);
	/* The test code rejects first codec selection request for mSBC. */
	ck_assert_int_eq(ba_transport_select_codec_sco(sco, HFP_CODEC_MSBC), -1);
	ck_assert_int_eq(ba_transport_select_codec_sco(sco, HFP_CODEC_MSBC), 0);
	pthread_mutex_unlock(&sco->codec_select_client_mtx);
	return NULL;
}
#endif

CK_START_TEST(test_rfcomm_hfp_ag) {

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
	struct ba_transport *sco = ba_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HFP_AG, ":test", "/sco", fds[0]);
	const int fd = fds[1];

	/* SLC initialization shall be started by the HF */

	/* supported features exchange: volume, codec, eSCO */
	ck_assert_int_eq(HFP_HF_FEAT_VOLUME | HFP_HF_FEAT_CODEC | HFP_HF_FEAT_ESCO, 656);
	ck_assert_rfcomm_send(fd, "AT+BRSF=656\r");
#if ENABLE_HFP_CODEC_SELECTION
	ck_assert_rfcomm_recv(fd, "\r\n+BRSF:2784\r\n");
#else
	ck_assert_rfcomm_recv(fd, "\r\n+BRSF:2272\r\n");
#endif
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");

#if ENABLE_HFP_CODEC_SELECTION
	/* verify that AG supports codec negotiation and eSCO */
	ck_assert_int_ne(2784 & (HFP_AG_FEAT_CODEC | HFP_AG_FEAT_ESCO), 0);
	/* codec negotiation */
#if ENABLE_MSBC && ENABLE_LC3_SWB
	ck_assert_rfcomm_send(fd, "AT+BAC=1,2,3\r");
#elif ENABLE_MSBC
	ck_assert_rfcomm_send(fd, "AT+BAC=1,2\r");
#elif ENABLE_LC3_SWB
	ck_assert_rfcomm_send(fd, "AT+BAC=1,3\r");
#endif
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
#endif

	/* AG indicators: retrieve supported indicators and their ordering */
	ck_assert_rfcomm_send(fd, "AT+CIND=?\r");
	ck_assert_rfcomm_recv(fd,
			"\r\n+CIND:(\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),"
			"(\"callheld\",(0-2)),(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))\r\n");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");

	/* AG indicators: retrieve the current status of the indicators */
	ck_assert_rfcomm_send(fd, "AT+CIND?\r");
	ck_assert_rfcomm_recv(fd, "\r\n+CIND:0,0,0,0,0,0,4\r\n");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");

	/* enable indicator status update */
	ck_assert_rfcomm_send(fd, "AT+CMER=3,0,0,1\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");

	/* SLC has been established */

	/* check support for setting microphone gain */
	ck_assert_rfcomm_send(fd, "AT+VGM=10\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.volume, 1);

	/* check vendor-specific command for microphone mute */
	ck_assert_rfcomm_send(fd, "AT+ANDROID=XHSMICMUTE,1\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.volume, 2);

	/* check vendor-specific command for ... */
	ck_assert_rfcomm_send(fd, "AT+ANDROID=BOOM\r");
	ck_assert_rfcomm_recv(fd, "\r\nERROR\r\n");

	/* check support for setting speaker gain */
	ck_assert_rfcomm_send(fd, "AT+VGS=13\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.volume, 3);

#if ENABLE_HFP_CODEC_SELECTION
	/* request codec connection setup */
	ck_assert_rfcomm_send(fd, "AT+BCC\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
	/* wait for codec selection */
# if ENABLE_LC3_SWB
	ck_assert_rfcomm_recv(fd, "\r\n+BCS:3\r\n");
	ck_assert_rfcomm_send(fd, "AT+BCS=3\r");
# elif ENABLE_MSBC
	ck_assert_rfcomm_recv(fd, "\r\n+BCS:2\r\n");
	ck_assert_rfcomm_send(fd, "AT+BCS=2\r");
# else
# error "Invalid codec configuration"
# endif
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.codec, 2);
#endif

#if ENABLE_MSBC

	/* use internal API to select the codec */
	GThread *th = g_thread_new(NULL, test_rfcomm_hfp_ag_switch_codecs, sco);

	ck_assert_rfcomm_recv(fd, "\r\n+BCS:1\r\n");
	ck_assert_rfcomm_send(fd, "AT+BCS=1\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");

	/* reject codec selection request */
	ck_assert_rfcomm_recv(fd, "\r\n+BCS:2\r\n");
	ck_assert_rfcomm_send(fd, "AT+BAC=1,2\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");

	ck_assert_rfcomm_recv(fd, "\r\n+BCS:2\r\n");
	ck_assert_rfcomm_send(fd, "AT+BCS=2\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");

	g_thread_join(th);

#endif

	config.battery.level = 100;
	/* use internal API to update battery level */
	ba_rfcomm_send_signal(sco->sco.rfcomm, BA_RFCOMM_SIGNAL_UPDATE_BATTERY);
	ck_assert_rfcomm_recv(fd, "\r\n+CIEV:7,5\r\n");

	/* disable all indicators */
	ck_assert_rfcomm_send(fd, "AT+BIA=0,0,0,0,0,0,0,0,0,0\r");
	ck_assert_rfcomm_recv(fd, "\r\nOK\r\n");

	/* battery level indicator shall not be reported */
	ba_rfcomm_send_signal(sco->sco.rfcomm, BA_RFCOMM_SIGNAL_UPDATE_BATTERY);
	ck_assert_rfcomm_recv(fd, "");

	const int level = -1000;
	struct ba_transport_pcm *pcm = &sco->sco.pcm_spk;
	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_volume_set(&pcm->volume[0], &level, NULL, NULL);
	pthread_mutex_unlock(&pcm->mutex);
	/* use internal API to update volume */
	ba_transport_pcm_volume_sync(pcm, BA_DBUS_PCM_UPDATE_VOLUME);
	ck_assert_rfcomm_recv(fd, "\r\n+VGS:7\r\n");

	ba_transport_destroy(sco);
	close(fd);

} CK_END_TEST

CK_START_TEST(test_rfcomm_hfp_hf) {

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
	struct ba_transport *sco = ba_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HFP_HF, ":test", "/sco", fds[0]);
	const int fd = fds[1];

	/* SLC initialization shall be started by the HF */

	/* supported features exchange */
#if ENABLE_HFP_CODEC_SELECTION
	ck_assert_rfcomm_recv(fd, "AT+BRSF=756\r");
#else
	ck_assert_rfcomm_recv(fd, "AT+BRSF=628\r");
#endif
	ck_assert_int_eq(HFP_AG_FEAT_CODEC | HFP_AG_FEAT_ESCO, 2560);
	ck_assert_rfcomm_send(fd, "\r\n+BRSF:2560\r\n");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

#if ENABLE_HFP_CODEC_SELECTION
	/* verify that HF supports codec negotiation and eSCO */
	ck_assert_int_ne(756 & (HFP_HF_FEAT_CODEC | HFP_HF_FEAT_ESCO), 0);
	/* codec negotiation */
# if ENABLE_MSBC && ENABLE_LC3_SWB
	ck_assert_rfcomm_recv(fd, "AT+BAC=1,2,3\r");
# elif ENABLE_MSBC
	ck_assert_rfcomm_recv(fd, "AT+BAC=1,2\r");
# elif ENABLE_LC3_SWB
	ck_assert_rfcomm_recv(fd, "AT+BAC=1,3\r");
# endif
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");
#endif

	/* AG indicators: retrieve supported indicators and their ordering */
	ck_assert_rfcomm_recv(fd, "AT+CIND=?\r");
	ck_assert_rfcomm_send(fd,
			"\r\n+CIND:(\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),"
			"(\"callheld\",(0-2)),(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))\r\n");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

	/* AG indicators: retrieve the current status of the indicators */
	ck_assert_rfcomm_recv(fd, "AT+CIND?\r");
	ck_assert_rfcomm_send(fd, "\r\n+CIND:0,0,0,0,0,0,4\r\n");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.battery, 1);

	/* enable indicator status update */
	ck_assert_rfcomm_recv(fd, "AT+CMER=3,0,0,1,0\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

	/* SLC has been established */

	/* report service, current signal strength and battery level */
	ck_assert_rfcomm_send(fd, "\r\n+CIEV:1,1\r\n");
	ck_assert_rfcomm_send(fd, "\r\n+CIEV:5,4\r\n");
	ck_assert_rfcomm_send(fd, "\r\n+CIEV:7,5\r\n");
	dbus_update_counters_wait(&dbus_update_counters.battery, 2);

	/* wait for initial microphone gain */
	ck_assert_rfcomm_recv(fd, "AT+VGM=15\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

	/* wait for initial speaker gain */
	ck_assert_rfcomm_recv(fd, "AT+VGS=15\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

	/* wait for initial vendor-specific battery level */
	ck_assert_rfcomm_recv(fd, "AT+XAPL=DEAD-C0DE-1234,6\r");
	ck_assert_rfcomm_send(fd, "\r\n+XAPL:TEST,6\r\n");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");
	ck_assert_rfcomm_recv(fd, "AT+IPHONEACCEV=2,1,8,2,0\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

#if ENABLE_HFP_CODEC_SELECTION

	/* wait for codec selection request */
	ck_assert_rfcomm_recv(fd, "AT+BCC\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");

	/* codec selection */
	ck_assert_rfcomm_send(fd, "\r\n+BCS:1\r\n");
	ck_assert_rfcomm_recv(fd, "AT+BCS=1\r");
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.codec, 2);

	/* codec selection: initial codec */
	ck_assert_rfcomm_send(fd, "\r\n+BCS:42\r\n");
# if ENABLE_MSBC && ENABLE_LC3_SWB
	ck_assert_rfcomm_recv(fd, "AT+BAC=1,2,3\r");
# elif ENABLE_MSBC
	ck_assert_rfcomm_recv(fd, "AT+BAC=1,2\r");
# elif ENABLE_LC3_SWB
	ck_assert_rfcomm_recv(fd, "AT+BAC=1,3\r");
# endif
	ck_assert_rfcomm_send(fd, "\r\nOK\r\n");
	dbus_update_counters_wait(&dbus_update_counters.codec, 2);

#endif

	/* check support for RING command */
	ck_assert_rfcomm_send(fd, "\r\nRING\r\n");
	usleep(5000);

	ba_transport_destroy(sco);
	close(fd);

} CK_END_TEST

CK_START_TEST(test_rfcomm_self_hfp_slc) {

	/* disable eSCO, so that codec negotiation is not performed */
	adapter->hci.features[2] &= ~LMP_TRSP_SCO;
	adapter->hci.features[3] &= ~LMP_ESCO;

	int fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

	struct ba_transport *ag = ba_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HFP_AG, ":test", "/sco/ag", fds[0]);
	struct ba_transport *hf = ba_transport_new_sco(device2,
			BA_TRANSPORT_PROFILE_HFP_HF, ":test", "/sco/hf", fds[1]);

	ck_assert_int_eq(ba_transport_get_codec(ag), HFP_CODEC_CVSD);
	ck_assert_int_eq(ba_transport_get_codec(hf), HFP_CODEC_CVSD);

	/* wait for codec selection (SLC established) signals */
	dbus_update_counters_wait(&dbus_update_counters.codec, 0 + (2 + 2));

	pthread_mutex_lock(&adapter->devices_mutex);
	ck_assert_int_eq(device1->ref_count, 1 + 1);
	ck_assert_int_eq(device2->ref_count, 1 + 1);
	pthread_mutex_unlock(&adapter->devices_mutex);

	ck_assert_int_eq(ba_transport_get_codec(ag), HFP_CODEC_CVSD);
	ck_assert_int_eq(ba_transport_get_codec(hf), HFP_CODEC_CVSD);

	debug("Audio gateway destroying");
	ba_transport_destroy(ag);
	/* The hf transport shall be destroyed by the "link lost" quirk. However,
	 * we have to wait "some" time before reference counter check, because
	 * this action is asynchronous from our point of view. */
	debug("Hands Free unreferencing");
	ba_transport_unref(hf);
	debug("Wait for asynchronous free");
	usleep(100000);

	pthread_mutex_lock(&adapter->devices_mutex);
	ck_assert_int_eq(device1->ref_count, 1);
	ck_assert_int_eq(device2->ref_count, 1);
	pthread_mutex_unlock(&adapter->devices_mutex);

} CK_END_TEST

void tc_setup(void) {

	config.battery.available = true;
	config.battery.level = 80;

	config.hfp.xapl_vendor_id = 0xDEAD,
	config.hfp.xapl_product_id = 0xC0DE,
	config.hfp.xapl_sw_version = 0x1234,

	memset(adapter->hci.features, 0, sizeof(adapter->hci.features));
	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;

	memset(&dbus_update_counters, 0, sizeof(dbus_update_counters));

}

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
	tcase_add_checked_fixture(tc, tc_setup, NULL);
	tcase_set_timeout(tc, 10);

	tcase_add_test(tc, test_rfcomm_hsp_ag);
	tcase_add_test(tc, test_rfcomm_hsp_hs);
	tcase_add_test(tc, test_rfcomm_hfp_ag);
	tcase_add_test(tc, test_rfcomm_hfp_hf);
	tcase_add_test(tc, test_rfcomm_self_hfp_slc);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	ba_device_unref(device1);
	ba_device_unref(device2);
	ba_adapter_unref(adapter);

	return nf == 0 ? 0 : 1;
}
