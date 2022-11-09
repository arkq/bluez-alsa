/*
 * test-ba.c
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

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
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
#include "bluez.h"
#include "sco.h"
#include "storage.h"
#include "shared/a2dp-codecs.h"
#include "shared/log.h"

#include "../src/ba-transport.c"

#define TEST_BLUEALSA_STORAGE_DIR "/tmp/bluealsa-test-ba-storage"

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
void a2dp_mpeg_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_mpeg_transport_start(struct ba_transport *t) { (void)t; return 0; }
void a2dp_sbc_transport_init(struct ba_transport *t) { (void)t; }
int a2dp_sbc_transport_start(struct ba_transport *t) { (void)t; return 0; }

void *ba_rfcomm_thread(struct ba_transport *t) { (void)t; return 0; }
void *sco_enc_thread(struct ba_transport_thread *th) { return sleep(3600), th; }
void *sco_dec_thread(struct ba_transport_thread *th) { return sleep(3600), th; }
int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); return 0; }
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)pcm, mask); }
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); }
struct ba_rfcomm *ba_rfcomm_new(struct ba_transport *sco, int fd) {
	debug("%s: %p", __func__, (void *)sco); (void)fd; return NULL; }
void ba_rfcomm_destroy(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); }
int ba_rfcomm_send_signal(struct ba_rfcomm *r, enum ba_rfcomm_signal sig) {
	debug("%s: %p: %#x", __func__, (void *)r, sig); return 0; }
bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s", __func__, current_dbus_sep_path); (void)sep;
	(void)error; return false; }

START_TEST(test_ba_adapter) {

	struct ba_adapter *a;

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_str_eq(a->hci.name, "hci0");
	ba_adapter_unref(a);

	ck_assert_ptr_ne(a = ba_adapter_new(5), NULL);
	ck_assert_int_eq(a->hci.dev_id, 5);
	ck_assert_str_eq(a->hci.name, "hci5");

	ck_assert_ptr_eq(ba_adapter_lookup(5), a);
	ba_adapter_unref(a);

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

	ck_assert_ptr_eq(ba_device_lookup(a, &addr), d);
	ba_device_unref(d);

	ba_device_unref(d);

} END_TEST

START_TEST(test_ba_transport) {

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
	ck_assert_int_eq(t->type.profile, BA_TRANSPORT_PROFILE_NONE);
	ck_assert_str_eq(t->bluez_dbus_owner, "/owner");
	ck_assert_str_eq(t->bluez_dbus_path, "/path");

	ck_assert_ptr_eq(ba_transport_lookup(d, "/path"), t);
	ba_transport_unref(t);

	ba_transport_unref(t);

} END_TEST

START_TEST(test_ba_transport_pcm_format) {

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

} END_TEST

START_TEST(test_ba_transport_pcm_volume) {

	struct ba_adapter *a;
	struct ba_device *d;
	struct ba_transport *t_a2dp;
	struct ba_transport *t_sco;
	bdaddr_t addr = { 0 };

	ck_assert_ptr_ne(a = ba_adapter_new(0), NULL);
	ck_assert_ptr_ne(d = ba_device_new(a, &addr), NULL);

	struct ba_transport_type ttype_a2dp = { .profile = BA_TRANSPORT_PROFILE_A2DP_SINK };
	struct a2dp_codec codec = { .dir = A2DP_SINK, .codec_id = A2DP_CODEC_SBC };
	a2dp_sbc_t configuration = { .channel_mode = SBC_CHANNEL_MODE_STEREO };
	ck_assert_ptr_ne(t_a2dp = ba_transport_new_a2dp(d, ttype_a2dp,
				"/owner", "/path/a2dp", &codec, &configuration), NULL);

	struct ba_transport_type ttype_sco = { .profile = BA_TRANSPORT_PROFILE_HFP_AG };
	ck_assert_ptr_ne(t_sco = ba_transport_new_sco(d, ttype_sco, "/owner", "/path/sco", -1), NULL);

	ba_adapter_unref(a);
	ba_device_unref(d);

	ck_assert_int_eq(t_a2dp->a2dp.pcm.max_bt_volume, 127);
	ck_assert_int_eq(t_a2dp->a2dp.pcm_bc.max_bt_volume, 127);

	ck_assert_int_eq(t_sco->sco.spk_pcm.max_bt_volume, 15);
	ck_assert_int_eq(t_sco->sco.mic_pcm.max_bt_volume, 15);

	ck_assert_int_eq(ba_transport_pcm_volume_bt_to_level(&t_a2dp->a2dp.pcm, 0), -9600);
	ck_assert_int_eq(ba_transport_pcm_volume_level_to_bt(&t_a2dp->a2dp.pcm, -9600), 0);

	ck_assert_int_eq(ba_transport_pcm_volume_bt_to_level(&t_a2dp->a2dp.pcm, 127), 0);
	ck_assert_int_eq(ba_transport_pcm_volume_level_to_bt(&t_a2dp->a2dp.pcm, 0), 127);

	ck_assert_int_eq(ba_transport_pcm_volume_bt_to_level(&t_sco->sco.spk_pcm, 0), -9600);
	ck_assert_int_eq(ba_transport_pcm_volume_level_to_bt(&t_sco->sco.spk_pcm, -9600), 0);

	ck_assert_int_eq(ba_transport_pcm_volume_bt_to_level(&t_sco->sco.spk_pcm, 15), 0);
	ck_assert_int_eq(ba_transport_pcm_volume_level_to_bt(&t_sco->sco.spk_pcm, 0), 15);

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
	ck_assert_ptr_ne(t = transport_new(d, "/owner", "/path"), NULL);
	t->release = test_cascade_free_transport_unref;

	ba_device_unref(d);
	ba_adapter_destroy(a);

} END_TEST

START_TEST(test_storage) {

	const char *storage_path = TEST_BLUEALSA_STORAGE_DIR "/00:11:22:33:44:55";
	const char *storage_data =
		"[/org/bluealsa/hci0/dev_00_11_22_33_44_55/a2dpsnk/source]\n"
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

	struct ba_transport_type ttype = { .profile = BA_TRANSPORT_PROFILE_A2DP_SINK };
	struct a2dp_codec codec = { .dir = A2DP_SINK, .codec_id = A2DP_CODEC_SBC };
	a2dp_sbc_t configuration = { .channel_mode = SBC_CHANNEL_MODE_STEREO };
	ck_assert_ptr_ne(t = ba_transport_new_a2dp(d, ttype,
				"/owner", "/path", &codec, &configuration), NULL);

	/* check if persistent storage was loaded */
	ck_assert_int_eq(t->a2dp.pcm.soft_volume, false);
	ck_assert_int_eq(t->a2dp.pcm.volume[0].level, -5600);
	ck_assert_int_eq(t->a2dp.pcm.volume[0].soft_mute, false);
	ck_assert_int_eq(t->a2dp.pcm.volume[1].level, -4800);
	ck_assert_int_eq(t->a2dp.pcm.volume[1].soft_mute, true);

	bool muted = true;
	int level = ba_transport_pcm_volume_bt_to_level(&t->a2dp.pcm, 100);
	ba_transport_pcm_volume_set(&t->a2dp.pcm.volume[0], &level, &muted, NULL);
	ba_transport_pcm_volume_set(&t->a2dp.pcm.volume[1], &level, &muted, NULL);

	ba_transport_unref(t);
	ba_adapter_unref(a);
	ba_device_unref(d);

	char buffer[1024] = { 0 };
	ck_assert_ptr_ne(f = fopen(storage_path, "r"), NULL);
	ck_assert_int_eq(fread(buffer, 1, sizeof(buffer), f), 212);
	ck_assert_int_eq(fclose(f), 0);

	const char *storage_data_new =
		"[/org/bluealsa/hci0/dev_00_11_22_33_44_55/a2dpsnk/source]\n"
		"SoftVolume=false\n"
		"Volume=-344;-344;\n"
		"Mute=true;true;\n"
		"\n"
		"[/org/bluealsa/hci0/dev_00_11_22_33_44_55/a2dpsnk/sink]\n"
		"SoftVolume=true\n"
		"Volume=0;0;\n"
		"Mute=false;false;\n";

	/* check if persistent storage was updated */
	ck_assert_str_eq(buffer, storage_data_new);

} END_TEST

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
	tcase_add_test(tc, test_ba_transport_pcm_format);
	tcase_add_test(tc, test_ba_transport_pcm_volume);
	tcase_add_test(tc, test_cascade_free);
	tcase_add_test(tc, test_storage);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
