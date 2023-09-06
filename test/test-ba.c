/*
 * test-ba.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <check.h>
#include <glib.h>

#include "a2dp.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluez.h"
#include "hfp.h"
#if ENABLE_OFONO
# include "ofono.h"
#endif
#include "storage.h"
#include "shared/a2dp-codecs.h"
#include "shared/log.h"

#include "../src/ba-transport.c"
#include "inc/check.inc"

#define TEST_BLUEALSA_STORAGE_DIR "/tmp/bluealsa-test-ba-storage"

void a2dp_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_transport_start(struct ba_transport *t) { (void)t; return 0; }
void *sco_enc_thread(struct ba_transport_pcm *t_pcm);

void *ba_rfcomm_thread(struct ba_transport *t) { (void)t; return 0; }
int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); (void)pcm; return 0; }
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)pcm, mask); (void)pcm; (void)mask; }
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); (void)pcm; }
struct ba_rfcomm *ba_rfcomm_new(struct ba_transport *sco, int fd) {
	debug("%s: %p", __func__, (void *)sco); (void)sco; (void)fd; return NULL; }
void ba_rfcomm_destroy(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); (void)r; }
int ba_rfcomm_send_signal(struct ba_rfcomm *r, enum ba_rfcomm_signal sig) {
	debug("%s: %p: %#x", __func__, (void *)r, sig); (void)r; (void)sig; return 0; }
bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s: %p", __func__, current_dbus_sep_path, sep);
	(void)current_dbus_sep_path; (void)sep; (void)error; return false; }
int ofono_call_volume_update(struct ba_transport *t) {
	debug("%s: %p", __func__, t); (void)t; return 0; }

CK_START_TEST(test_ba_adapter) {

	struct ba_adapter *a;

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_str_eq(a->hci.name, "hci0");

	ba_adapter_unref(a);
	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

	ck_assert_ptr_ne(a = ba_adapter_new(5), NULL);
	ck_assert_int_eq(a->hci.dev_id, 5);
	ck_assert_str_eq(a->hci.name, "hci5");

	ck_assert_ptr_eq(ba_adapter_lookup(5), a);
	ba_adapter_unref(a);

	ba_adapter_unref(a);
	ck_assert_ptr_eq(ba_adapter_lookup(5), NULL);

} CK_END_TEST

CK_START_TEST(test_ba_device) {

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

	ck_assert_ptr_eq(ba_device_lookup(a, &addr), d);
	ba_device_unref(d);

	ba_device_unref(d);
	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

} CK_END_TEST

CK_START_TEST(test_ba_transport) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	ck_assert_ptr_ne(t = transport_new(d, "/owner", "/path"), NULL);

	ba_adapter_unref(a);
	ba_device_unref(d);

	ck_assert_ptr_eq(t->d, d);
	ck_assert_int_eq(t->profile, BA_TRANSPORT_PROFILE_NONE);
	ck_assert_str_eq(t->bluez_dbus_owner, "/owner");
	ck_assert_str_eq(t->bluez_dbus_path, "/path");

	ck_assert_ptr_eq(ba_transport_lookup(d, "/path"), t);
	ba_transport_unref(t);

	ba_transport_unref(t);
	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

} CK_END_TEST

CK_START_TEST(test_ba_transport_sco_one_only) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t_sco_hsp;
	struct ba_transport *t_sco_hfp;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	t_sco_hsp = ba_transport_new_sco(d, BA_TRANSPORT_PROFILE_HSP_AG, "/owner", "/path/sco", -1);
	ck_assert_ptr_ne(t_sco_hsp, NULL);

	t_sco_hfp = ba_transport_new_sco(d, BA_TRANSPORT_PROFILE_HFP_AG, "/owner", "/path/sco", -1);
	ck_assert_ptr_eq(t_sco_hfp, NULL);
	ck_assert_int_eq(errno, EBUSY);

	ba_transport_unref(t_sco_hsp);

	ba_adapter_unref(a);
	ba_device_unref(d);
	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

} CK_END_TEST

CK_START_TEST(test_ba_transport_sco_default_codec) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t_sco;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	t_sco = ba_transport_new_sco(d, BA_TRANSPORT_PROFILE_HSP_AG, "/owner", "/path/sco", -1);
	ck_assert_int_eq(ba_transport_get_codec(t_sco), HFP_CODEC_CVSD);
	ba_transport_unref(t_sco);

#if ENABLE_MSBC

	a->hci.features[2] = LMP_TRSP_SCO;
	a->hci.features[3] = LMP_ESCO;

	config.hfp.codecs.msbc = true;
	t_sco = ba_transport_new_sco(d, BA_TRANSPORT_PROFILE_HFP_AG, "/owner", "/path/sco", -1);
	ck_assert_int_eq(ba_transport_get_codec(t_sco), HFP_CODEC_UNDEFINED);
	ba_transport_unref(t_sco);

	config.hfp.codecs.msbc = false;
	t_sco = ba_transport_new_sco(d, BA_TRANSPORT_PROFILE_HFP_AG, "/owner", "/path/sco", -1);
	ck_assert_int_eq(ba_transport_get_codec(t_sco), HFP_CODEC_CVSD);
	ba_transport_unref(t_sco);

#else
	t_sco = ba_transport_new_sco(d, BA_TRANSPORT_PROFILE_HFP_AG, "/owner", "/path/sco", -1);
	ck_assert_int_eq(ba_transport_get_codec(t_sco), HFP_CODEC_CVSD);
	ba_transport_unref(t_sco);
#endif

	ba_adapter_unref(a);
	ba_device_unref(d);
	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

} CK_END_TEST

static void *cleanup_thread(struct ba_transport_pcm *t_pcm) {
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	ba_transport_pcm_thread_cleanup(t_pcm);
	return NULL;
}

CK_START_TEST(test_ba_transport_threads_sync_termination) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t_sco;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	t_sco = ba_transport_new_sco(d, BA_TRANSPORT_PROFILE_HSP_AG, "/owner", "/path/sco", -1);
	ck_assert_ptr_ne(t_sco, NULL);

	t_sco->bt_fd = 0;
	t_sco->mtu_read = 48;
	t_sco->mtu_write = 48;

	ck_assert_int_eq(ba_transport_pcm_start(t_sco->thread_enc.pcm, sco_enc_thread, "enc", true), 0);
	ck_assert_int_eq(ba_transport_thread_state_wait_running(&t_sco->thread_enc), 0);

	ck_assert_int_eq(ba_transport_pcm_start(t_sco->thread_dec.pcm, cleanup_thread, "dec", false), 0);
	ck_assert_int_eq(ba_transport_thread_state_wait_running(&t_sco->thread_dec), -1);

	ck_assert_int_eq(ba_transport_thread_state_wait_terminated(&t_sco->thread_enc), 0);
	ck_assert_int_eq(ba_transport_thread_state_wait_terminated(&t_sco->thread_dec), 0);

	ba_adapter_unref(a);
	ba_device_unref(d);
	ba_transport_unref(t_sco);
	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

} CK_END_TEST

CK_START_TEST(test_ba_transport_pcm_format) {

	uint16_t format_u8 = BA_TRANSPORT_PCM_FORMAT_U8;
	uint16_t format_s32_4le = BA_TRANSPORT_PCM_FORMAT_S32_4LE;

	ck_assert_int_eq(format_u8, 0x0108);
	ck_assert_int_eq(BA_TRANSPORT_PCM_FORMAT_SIGN(format_u8), 0);
	ck_assert_int_eq(BA_TRANSPORT_PCM_FORMAT_WIDTH(format_u8), 8);
	ck_assert_int_eq(BA_TRANSPORT_PCM_FORMAT_BYTES(format_u8), 1);
	ck_assert_int_eq(BA_TRANSPORT_PCM_FORMAT_ENDIAN(format_u8), 0);

	ck_assert_int_eq(format_s32_4le, 0x8420);
	ck_assert_int_eq(BA_TRANSPORT_PCM_FORMAT_SIGN(format_s32_4le), 1);
	ck_assert_int_eq(BA_TRANSPORT_PCM_FORMAT_WIDTH(format_s32_4le), 32);
	ck_assert_int_eq(BA_TRANSPORT_PCM_FORMAT_BYTES(format_s32_4le), 4);
	ck_assert_int_eq(BA_TRANSPORT_PCM_FORMAT_ENDIAN(format_s32_4le), 0);

} CK_END_TEST

CK_START_TEST(test_ba_transport_pcm_volume) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t_a2dp;
	struct ba_transport *t_sco;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	struct a2dp_codec codec = { .dir = A2DP_SINK, .codec_id = A2DP_CODEC_SBC };
	a2dp_sbc_t configuration = { .channel_mode = SBC_CHANNEL_MODE_STEREO };
	ck_assert_ptr_ne(t_a2dp = ba_transport_new_a2dp(d,
				BA_TRANSPORT_PROFILE_A2DP_SINK, "/owner", "/path/a2dp", &codec,
				&configuration), NULL);

	ck_assert_ptr_ne(t_sco = ba_transport_new_sco(d,
				BA_TRANSPORT_PROFILE_HFP_AG, "/owner", "/path/sco", -1), NULL);

	ba_adapter_unref(a);
	ba_device_unref(d);

	ck_assert_int_eq(ba_transport_pcm_volume_range_to_level(0, BLUEZ_A2DP_VOLUME_MAX), -9600);
	ck_assert_int_eq(ba_transport_pcm_volume_level_to_range(-9600, BLUEZ_A2DP_VOLUME_MAX), 0);

	ck_assert_int_eq(ba_transport_pcm_volume_range_to_level(127, BLUEZ_A2DP_VOLUME_MAX), 0);
	ck_assert_int_eq(ba_transport_pcm_volume_level_to_range(0, BLUEZ_A2DP_VOLUME_MAX), 127);

	ck_assert_int_eq(ba_transport_pcm_volume_range_to_level(0, HFP_VOLUME_GAIN_MAX), -9600);
	ck_assert_int_eq(ba_transport_pcm_volume_level_to_range(-9600, HFP_VOLUME_GAIN_MAX), 0);

	ck_assert_int_eq(ba_transport_pcm_volume_range_to_level(15, HFP_VOLUME_GAIN_MAX), 0);
	ck_assert_int_eq(ba_transport_pcm_volume_level_to_range(0, HFP_VOLUME_GAIN_MAX), 15);

	ba_transport_unref(t_a2dp);
	ba_transport_unref(t_sco);

	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

} CK_END_TEST

static int test_cascade_free_transport_unref(struct ba_transport *t) {
	return ba_transport_unref(t), 0;
}

CK_START_TEST(test_cascade_free) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);
	ck_assert_ptr_ne(t = transport_new(d, "/owner", "/path"), NULL);

	t->bt_fd = 0;  /* release() is called for acquired transport only */
	t->release = test_cascade_free_transport_unref;

	ba_device_unref(d);
	ba_adapter_destroy(a);

	/* verify that cascade free was performed */
	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

} CK_END_TEST

CK_START_TEST(test_storage) {

	const char *storage_path = TEST_BLUEALSA_STORAGE_DIR "/00:11:22:33:44:55";
	const char *storage_data =
		"[/org/bluealsa/hci0/dev_00_11_22_33_44_55/a2dpsnk/source]\n"
		"DelayAdjustments=SBC:-200\n"
		"SoftVolume=false\n"
		"Volume=-5600;-4800;\n"
		"Mute=false;true;\n";

	FILE *f;
	ck_assert_ptr_ne(f = fopen(storage_path, "w"), NULL);
	ck_assert_int_eq(fwrite(storage_data, strlen(storage_data), 1, f), 1);
	ck_assert_int_eq(fclose(f), 0);

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t;

	bdaddr_t addr;
	str2ba(&storage_path[sizeof(TEST_BLUEALSA_STORAGE_DIR)], &addr);

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	struct a2dp_codec codec = { .dir = A2DP_SINK, .codec_id = A2DP_CODEC_SBC };
	a2dp_sbc_t configuration = { .channel_mode = SBC_CHANNEL_MODE_STEREO };
	ck_assert_ptr_ne(t = ba_transport_new_a2dp(d,
				BA_TRANSPORT_PROFILE_A2DP_SINK, "/owner", "/path", &codec,
				&configuration), NULL);

	/* check if persistent storage was loaded */
	ck_assert_int_eq(t->a2dp.pcm.soft_volume, false);
	ck_assert_int_eq(t->a2dp.pcm.volume[0].level, -5600);
	ck_assert_int_eq(t->a2dp.pcm.volume[0].soft_mute, false);
	ck_assert_int_eq(t->a2dp.pcm.volume[1].level, -4800);
	ck_assert_int_eq(t->a2dp.pcm.volume[1].soft_mute, true);
	ck_assert_int_eq(ba_transport_pcm_delay_adjustment_get(&t->a2dp.pcm), -200);

	bool muted = true;
	int level = ba_transport_pcm_volume_range_to_level(100, BLUEZ_A2DP_VOLUME_MAX);
	ba_transport_pcm_volume_set(&t->a2dp.pcm.volume[0], &level, &muted, NULL);
	ba_transport_pcm_volume_set(&t->a2dp.pcm.volume[1], &level, &muted, NULL);
	ba_transport_pcm_delay_adjustment_set(&t->a2dp.pcm, A2DP_CODEC_SBC, 140);

	ba_adapter_unref(a);
	ba_device_unref(d);
	ba_transport_unref(t);
	ck_assert_ptr_eq(ba_adapter_lookup(0), NULL);

	char buffer[1024] = { 0 };
	ck_assert_ptr_ne(f = fopen(storage_path, "r"), NULL);
	ck_assert_int_eq(fread(buffer, 1, sizeof(buffer), f), 256);
	ck_assert_int_eq(fclose(f), 0);

	const char *storage_data_new =
		"[/org/bluealsa/hci0/dev_00_11_22_33_44_55/a2dpsnk/source]\n"
		"DelayAdjustments=SBC:140;\n"
		"SoftVolume=false\n"
		"Volume=-344;-344;\n"
		"Mute=true;true;\n"
		"\n"
		"[/org/bluealsa/hci0/dev_00_11_22_33_44_55/a2dpsnk/sink]\n"
		"DelayAdjustments=\n"
		"SoftVolume=true\n"
		"Volume=0;0;\n"
		"Mute=false;false;\n";

	/* check if persistent storage was updated */
	ck_assert_str_eq(buffer, storage_data_new);

} CK_END_TEST

int main(void) {

	assert(mkdir(TEST_BLUEALSA_STORAGE_DIR, 0755) == 0 || errno == EEXIST);
	assert(storage_init(TEST_BLUEALSA_STORAGE_DIR) == 0);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_ba_adapter);
	tcase_add_test(tc, test_ba_device);
	tcase_add_test(tc, test_ba_transport);
	tcase_add_test(tc, test_ba_transport_sco_one_only);
	tcase_add_test(tc, test_ba_transport_sco_default_codec);
	tcase_add_test(tc, test_ba_transport_threads_sync_termination);
	tcase_add_test(tc, test_ba_transport_pcm_format);
	tcase_add_test(tc, test_ba_transport_pcm_volume);
	tcase_add_test(tc, test_cascade_free);
	tcase_add_test(tc, test_storage);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
