/*
 * test-alsa-ctl.c
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

#include <errno.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>
#include <alsa/asoundlib.h>

#include "inc/check.inc"
#include "inc/mock.inc"
#include "inc/preload.inc"
#include "inc/spawn.inc"

static int snd_ctl_open_bluealsa(
		snd_ctl_t **ctlp,
		const char *service,
		const char *extra_config,
		int mode) {

	char buffer[256];
	snd_config_t *conf = NULL;
	snd_input_t *input = NULL;
	int err;

	sprintf(buffer,
			"ctl.bluealsa {\n"
			"  type bluealsa\n"
			"  service \"org.bluealsa.%s\"\n"
			"  %s\n"
			"}\n", service, extra_config);

	if ((err = snd_config_top(&conf)) < 0)
		goto fail;
	if ((err = snd_input_buffer_open(&input, buffer, strlen(buffer))) != 0)
		goto fail;
	if ((err = snd_config_load(conf, input)) != 0)
		goto fail;
	err = snd_ctl_open_lconf(ctlp, "bluealsa", mode, conf);

fail:
	if (conf != NULL)
		snd_config_delete(conf);
	if (input != NULL)
		snd_input_close(input);
	return err;
}

static int test_ctl_open(struct spawn_process *sp_ba_mock, snd_ctl_t **ctl, int mode) {
	const char *service = "test";
	if (spawn_bluealsa_mock(sp_ba_mock, service, true,
				"--timeout=1000",
				"--profile=a2dp-source",
				"--profile=a2dp-sink",
				"--profile=hfp-ag",
				NULL) == -1)
		return -1;
	return snd_ctl_open_bluealsa(ctl, service, "", mode);
}

static int test_pcm_close(struct spawn_process *sp_ba_mock, snd_ctl_t *ctl) {
	int rv = 0;
	if (ctl != NULL)
		rv = snd_ctl_close(ctl);
	if (sp_ba_mock != NULL) {
		spawn_terminate(sp_ba_mock, 0);
		spawn_close(sp_ba_mock, NULL);
	}
	return rv;
}

CK_START_TEST(test_controls) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	ck_assert_int_eq(test_ctl_open(&sp_ba_mock, &ctl, 0), 0);

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 12);
	ck_assert_int_eq(snd_ctl_elem_list_alloc_space(elems, 12), 0);
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);

	ck_assert_int_eq(snd_ctl_elem_list_get_used(elems), 12);

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 0), "12:34:56:78:9A:BC A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 1), "12:34:56:78:9A:BC A2DP Playback Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 2), "12:34:56:78:9A:BC A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 3), "12:34:56:78:9A:BC A2DP Capture Volume");

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 4), "12:34:56:78:9A:BC SCO Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 5), "12:34:56:78:9A:BC SCO Playback Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 6), "12:34:56:78:9A:BC SCO Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 7), "12:34:56:78:9A:BC SCO Capture Volume");

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 8), "23:45:67:89:AB:CD A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 9), "23:45:67:89:AB:CD A2DP Playback Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 10), "23:45:67:89:AB:CD A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 11), "23:45:67:89:AB:CD A2DP Capture Volume");

	snd_ctl_elem_list_free_space(elems);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_controls_battery) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, true,
				"--timeout=1000",
				"--profile=hsp-ag",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"battery \"yes\"\n", 0), 0);

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 5);
	ck_assert_int_eq(snd_ctl_elem_list_alloc_space(elems, 5), 0);
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);

	ck_assert_int_eq(snd_ctl_elem_list_get_used(elems), 5);

	/* battery control element shall be last */
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 4), "23:45:67:89:AB:CD | Battery Playback Volume");

	snd_ctl_elem_value_t *elem;
	snd_ctl_elem_value_alloca(&elem);
	snd_ctl_elem_value_set_numid(elem, snd_ctl_elem_list_get_numid(elems, 4));

	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem, 0), 75);
	/* battery control element is read-only */
	ck_assert_int_eq(snd_ctl_elem_write(ctl, elem), -EINVAL);

	snd_ctl_elem_list_free_space(elems);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_controls_extended) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, true,
				"--timeout=1000",
				"--profile=a2dp-source",
				"--profile=hfp-ag",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"extended \"yes\"\n", 0), 0);

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 15);
	ck_assert_int_eq(snd_ctl_elem_list_alloc_space(elems, 15), 0);
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);

	/* codec control element shall be after playback/capture elements */
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 3), "12:34:56:78:9A:BC A2DP Codec Enum");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 10), "12:34:56:78:9A:BC SCO Codec Enum");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 14), "23:45:67:89:AB:CD A2DP Codec Enum");

	bool has_msbc = false;
#if ENABLE_MSBC
	has_msbc = true;
#endif

	snd_ctl_elem_info_t *info;
	snd_ctl_elem_info_alloca(&info);

	/* 12:34:56:78:9A:BC SCO Codec Enum */
	snd_ctl_elem_info_set_numid(info, snd_ctl_elem_list_get_numid(elems, 10));
	ck_assert_int_eq(snd_ctl_elem_info(ctl, info), 0);
	ck_assert_int_eq(snd_ctl_elem_info_get_items(info), has_msbc ? 2 : 1);
	snd_ctl_elem_info_set_item(info, 0);
	ck_assert_int_eq(snd_ctl_elem_info(ctl, info), 0);
	ck_assert_str_eq(snd_ctl_elem_info_get_item_name(info), "CVSD");
#if ENABLE_MSBC
	snd_ctl_elem_info_set_item(info, 1);
	ck_assert_int_eq(snd_ctl_elem_info(ctl, info), 0);
	ck_assert_str_eq(snd_ctl_elem_info_get_item_name(info), "mSBC");
#endif

	snd_ctl_elem_value_t *elem;
	snd_ctl_elem_value_alloca(&elem);

	/* 12:34:56:78:9A:BC A2DP Codec Enum */
	snd_ctl_elem_value_set_numid(elem, snd_ctl_elem_list_get_numid(elems, 3));
	/* get currently selected A2DP codec */
	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_enumerated(elem, 0), 0);
	/* select A2DP SBC codec */
	snd_ctl_elem_value_set_enumerated(elem, 0, 0);
	/* write reports 0 because we are setting currently selected codec */
	ck_assert_int_eq(snd_ctl_elem_write(ctl, elem), 0);

	/* 12:34:56:78:9A:BC SCO Codec Enum */
	snd_ctl_elem_value_set_numid(elem, snd_ctl_elem_list_get_numid(elems, 10));
	/* get currently selected SCO codec */
	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_enumerated(elem, 0), has_msbc ? 1 : 0);
#if ENABLE_MSBC
	/* select SCO CVSD codec */
	snd_ctl_elem_value_set_enumerated(elem, 0, 0);
	ck_assert_int_eq(snd_ctl_elem_write(ctl, elem), 1);
#endif

	snd_ctl_elem_list_free_space(elems);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_bidirectional_a2dp) {
#if ENABLE_FASTSTREAM

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, true,
				"--timeout=1000",
				"--profile=a2dp-source",
				"--profile=a2dp-sink",
				"--codec=FastStream",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"bttransport \"yes\"\n", 0), 0);

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 10);
	ck_assert_int_eq(snd_ctl_elem_list_alloc_space(elems, 10), 0);
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 4), "23:45:67:89:AB:C A2DP-SRC Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 5), "23:45:67:89:AB:C A2DP-SRC Playback Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 6), "23:45:67:89:AB:C A2DP-SRC Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 7), "23:45:67:89:AB:C A2DP-SRC Capture Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 8), "23:45:67:89:AB:C A2DP-SNK Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 9), "23:45:67:89:AB:C A2DP-SNK Capture Volume");

	snd_ctl_elem_list_free_space(elems);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

#endif
} CK_END_TEST

CK_START_TEST(test_device_name_duplicates) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, true,
				"--timeout=1000",
				"--profile=a2dp-source",
				"--device-name=12:34:56:78:9A:BC:Long Bluetooth Device Name",
				"--device-name=23:45:67:89:AB:CD:Long Bluetooth Device Name",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service, "", 0), 0);

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 4);
	ck_assert_int_eq(snd_ctl_elem_list_alloc_space(elems, 4), 0);
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 0), "Long Bluetooth Devi #1 A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 1), "Long Bluetooth Devi #1 A2DP Playback Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 2), "Long Bluetooth Devi #2 A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 3), "Long Bluetooth Devi #2 A2DP Playback Volume");

	snd_ctl_elem_list_free_space(elems);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_mute_and_volume) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	ck_assert_int_eq(test_ctl_open(&sp_ba_mock, &ctl, 0), 0);

	snd_ctl_elem_value_t *elem_switch;
	snd_ctl_elem_value_alloca(&elem_switch);
	/* 23:45:67:89:AB:CD A2DP Playback Switch */
	snd_ctl_elem_value_set_numid(elem_switch, 9);

	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem_switch), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_boolean(elem_switch, 0), 1);
	ck_assert_int_eq(snd_ctl_elem_value_get_boolean(elem_switch, 1), 1);

	snd_ctl_elem_value_set_boolean(elem_switch, 0, 0);
	snd_ctl_elem_value_set_boolean(elem_switch, 1, 0);
	ck_assert_int_gt(snd_ctl_elem_write(ctl, elem_switch), 0);

	snd_ctl_elem_value_t *elem_volume;
	snd_ctl_elem_value_alloca(&elem_volume);
	/* 23:45:67:89:AB:CD A2DP Playback Volume */
	snd_ctl_elem_value_set_numid(elem_volume, 10);

	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem_volume), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 0), 127);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 1), 127);

	snd_ctl_elem_value_set_integer(elem_volume, 0, 42);
	snd_ctl_elem_value_set_integer(elem_volume, 1, 42);
	ck_assert_int_gt(snd_ctl_elem_write(ctl, elem_volume), 0);

	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem_volume), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 0), 42);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 1), 42);

	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_volume_db_range) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	ck_assert_int_eq(test_ctl_open(&sp_ba_mock, &ctl, 0), 0);

	snd_ctl_elem_id_t *elem;
	snd_ctl_elem_id_alloca(&elem);
	/* 12:34:56:78:9A:BC A2DP Playback Volume */
	snd_ctl_elem_id_set_numid(elem, 2);

	long min, max;
	ck_assert_int_eq(snd_ctl_get_dB_range(ctl, elem, &min, &max), 0);
	ck_assert_int_eq(min, -9600);
	ck_assert_int_eq(max, 0);

	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_single_device) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, true,
				"--timeout=1000",
				"--profile=a2dp-source",
				"--profile=a2dp-sink",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"device \"00:00:00:00:00:00\"", 0), 0);

	snd_ctl_card_info_t *info;
	snd_ctl_card_info_alloca(&info);

	ck_assert_int_eq(snd_ctl_card_info(ctl, info), 0);
	ck_assert_str_eq(snd_ctl_card_info_get_name(info), "23:45:67:89:AB:CD");

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 4);
	ck_assert_int_eq(snd_ctl_elem_list_alloc_space(elems, 4), 0);
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 0), "A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 1), "A2DP Playback Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 2), "A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 3), "A2DP Capture Volume");

	snd_ctl_elem_list_free_space(elems);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_single_device_not_connected) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, true,
				"--timeout=1000",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"device \"00:00:00:00:00:00\"", 0), -ENODEV);

	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_single_device_no_such_device) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, true,
				"--timeout=1000",
				"--profile=a2dp-source",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"device \"DE:AD:12:34:56:78\"", 0), -ENODEV);

	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_single_device_non_dynamic) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, true,
				"--timeout=0",
				"--profile=a2dp-sink",
				"--profile=hsp-ag",
				"--fuzzing=500",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"device \"23:45:67:89:AB:CD\"\n"
				"dynamic \"no\"\n", 0), 0);
	ck_assert_int_eq(snd_ctl_subscribe_events(ctl, 1), 0);

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	snd_ctl_event_t *event;
	snd_ctl_event_malloc(&event);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 6);

	snd_ctl_elem_value_t *elem_volume;
	snd_ctl_elem_value_alloca(&elem_volume);
	/* A2DP Capture Volume */
	snd_ctl_elem_value_set_numid(elem_volume, 2);

	snd_ctl_elem_value_set_integer(elem_volume, 0, 42);
	ck_assert_int_gt(snd_ctl_elem_write(ctl, elem_volume), 0);

	/* check whether element value was updated */
	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem_volume), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 0), 42);

	/* Process events until we will be notified about A2DP profile disconnection.
	 * We shall get 2 events from previous value update and 2 events for profile
	 * disconnection (one event per switch/volume element). */
	for (size_t events = 0; events < 4;) {
		ck_assert_int_eq(snd_ctl_wait(ctl, 750), 1);
		while (snd_ctl_read(ctl, event) == 1)
			events++;
	}

	/* the number of elements shall not change */
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 6);

	/* element shall be "deactivated" */
	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem_volume), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 0), 0);

	snd_ctl_elem_value_set_integer(elem_volume, 0, 42);
	ck_assert_int_gt(snd_ctl_elem_write(ctl, elem_volume), 0);

	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem_volume), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 0), 0);

	snd_ctl_event_free(event);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_notifications) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;

	const char *service = "test";
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, service, false,
				"--timeout=10000",
				"--profile=a2dp-source",
				"--profile=hfp-ag",
				"--fuzzing=250",
				NULL), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"battery \"yes\"\n", 0), 0);

	snd_ctl_event_t *event;
	snd_ctl_event_malloc(&event);

	ck_assert_int_eq(snd_ctl_subscribe_events(ctl, 1), 0);

	size_t events = 0;
	while (snd_ctl_wait(ctl, 500) == 1)
		while (snd_ctl_read(ctl, event) == 1) {
			ck_assert_int_eq(snd_ctl_event_get_type(event), SND_CTL_EVENT_ELEM);
			events++;
		}

	ck_assert_int_eq(snd_ctl_subscribe_events(ctl, 0), 0);

	size_t events_update_codec = 0;
#if ENABLE_MSBC
	events_update_codec += 4;
#endif

	/* Processed events:
	 * - 0 removes; 2 new elems (12:34:... A2DP)
	 * - 2 removes; 4 new elems (12:34:... A2DP, 23:45:... A2DP)
	 * - 4 removes; 7 new elems (2x A2DP, SCO playback, battery)
	 * - 7 removes; 9 new elems (2x A2DP, SCO playback/capture, battery)
	 * - 4 updates (SCO codec update if mSBC is supported)
	 *
	 * XXX: It is possible that the battery element (RFCOMM D-Bus path) will not
	 *      be exported in time. In such case, the number of events will be less
	 *      by 2 when RFCOMM D-Bus path is not available during the playback SCO
	 *      addition and less by another 1 when the path is not available during
	 *      the capture SCO addition. We shall account for this in the test, as
	 *      it is not an error. */
	if (events == (35 + events_update_codec - 2) ||
			events == (35 + events_update_codec - 2 - 1))
		events = 35 + events_update_codec;
	ck_assert_int_eq(events, (0 + 2) + (2 + 4) + (4 + 7) + (7 + 9) + events_update_codec);

	snd_ctl_event_free(event);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, ctl), 0);

} CK_END_TEST

CK_START_TEST(test_alsa_high_level_control_interface) {

	struct spawn_process sp_ba_mock;
	snd_ctl_t *ctl = NULL;
	snd_hctl_t *hctl = NULL;

	ck_assert_int_eq(test_ctl_open(&sp_ba_mock, &ctl, 0), 0);
	ck_assert_int_eq(snd_hctl_open_ctl(&hctl, ctl), 0);

	ck_assert_int_eq(snd_hctl_load(hctl), 0);
	ck_assert_int_eq(snd_hctl_get_count(hctl), 12);
	ck_assert_int_eq(snd_hctl_free(hctl), 0);

	ck_assert_int_eq(snd_hctl_close(hctl), 0);
	ck_assert_int_eq(test_pcm_close(&sp_ba_mock, NULL), 0);

} CK_END_TEST

int main(int argc, char *argv[], char *envp[]) {
	preload(argc, argv, envp, ".libs/aloader.so");

	char *argv_0 = strdup(argv[0]);
	snprintf(bluealsa_mock_path, sizeof(bluealsa_mock_path),
			"%s/mock/bluealsa-mock", dirname(argv_0));

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_controls);
	tcase_add_test(tc, test_controls_battery);
	tcase_add_test(tc, test_controls_extended);
	tcase_add_test(tc, test_bidirectional_a2dp);
	tcase_add_test(tc, test_device_name_duplicates);
	tcase_add_test(tc, test_mute_and_volume);
	tcase_add_test(tc, test_volume_db_range);
	tcase_add_test(tc, test_single_device);
	tcase_add_test(tc, test_single_device_not_connected);
	tcase_add_test(tc, test_single_device_no_such_device);
	tcase_add_test(tc, test_single_device_non_dynamic);
	tcase_add_test(tc, test_notifications);
	tcase_add_test(tc, test_alsa_high_level_control_interface);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	free(argv_0);

	return nf == 0 ? 0 : 1;
}
