/*
 * test-alsa-ctl.c
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

#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include <check.h>
#include <alsa/asoundlib.h>

#include "shared/defs.h"

#include "inc/preload.inc"
#include "inc/server.inc"

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
			"  battery true\n"
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

static int test_ctl_open(pid_t *pid, snd_ctl_t **ctl, int mode) {
	const char *service = "test";
	if ((*pid = spawn_bluealsa_server(service, 1, true, false, true, true, true, false)) == -1)
		return -1;
	return snd_ctl_open_bluealsa(ctl, service, "", mode);
}

static int test_pcm_close(pid_t pid, snd_ctl_t *ctl) {
	int rv = 0;
	if (ctl != NULL)
		rv = snd_ctl_close(ctl);
	if (pid != -1) {
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
	}
	return rv;
}

START_TEST(test_controls) {

	snd_ctl_t *ctl = NULL;
	pid_t pid = -1;

	ck_assert_int_eq(test_ctl_open(&pid, &ctl, 0), 0);

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 12);
	ck_assert_int_eq(snd_ctl_elem_list_alloc_space(elems, 12), 0);
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);

	ck_assert_int_eq(snd_ctl_elem_list_get_used(elems), 12);

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 0), "12:34:56:78:9A:BC - A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 1), "12:34:56:78:9A:BC - A2DP Capture Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 2), "12:34:56:78:9A:BC - A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 3), "12:34:56:78:9A:BC - A2DP Playback Volume");

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 4), "12:34:56:78:9A:BC - SCO Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 5), "12:34:56:78:9A:BC - SCO Capture Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 6), "12:34:56:78:9A:BC - SCO Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 7), "12:34:56:78:9A:BC - SCO Playback Volume");

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 8), "23:45:67:89:AB:CD - A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 9), "23:45:67:89:AB:CD - A2DP Capture Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 10), "23:45:67:89:AB:CD - A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 11), "23:45:67:89:AB:CD - A2DP Playback Volume");

	ck_assert_int_eq(test_pcm_close(pid, ctl), 0);

} END_TEST

START_TEST(test_mute_and_volume) {

	snd_ctl_t *ctl = NULL;
	pid_t pid = -1;

	ck_assert_int_eq(test_ctl_open(&pid, &ctl, 0), 0);

	snd_ctl_elem_value_t *elem_switch;
	snd_ctl_elem_value_alloca(&elem_switch);
	/* 23:45:67:89:AB:CD - A2DP Playback Switch */
	snd_ctl_elem_value_set_numid(elem_switch, 11);

	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem_switch), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_boolean(elem_switch, 0), 1);
	ck_assert_int_eq(snd_ctl_elem_value_get_boolean(elem_switch, 1), 1);

	snd_ctl_elem_value_set_boolean(elem_switch, 0, 0);
	snd_ctl_elem_value_set_boolean(elem_switch, 1, 0);
	ck_assert_int_gt(snd_ctl_elem_write(ctl, elem_switch), 0);

	snd_ctl_elem_value_t *elem_volume;
	snd_ctl_elem_value_alloca(&elem_volume);
	/* 23:45:67:89:AB:CD - A2DP Playback Switch */
	snd_ctl_elem_value_set_numid(elem_volume, 12);

	ck_assert_int_eq(snd_ctl_elem_read(ctl, elem_volume), 0);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 0), 127);
	ck_assert_int_eq(snd_ctl_elem_value_get_integer(elem_volume, 1), 127);

	snd_ctl_elem_value_set_integer(elem_volume, 0, 42);
	snd_ctl_elem_value_set_integer(elem_volume, 1, 42);
	ck_assert_int_gt(snd_ctl_elem_write(ctl, elem_volume), 0);

	ck_assert_int_eq(test_pcm_close(pid, ctl), 0);

} END_TEST

START_TEST(test_volume_db_range) {

	snd_ctl_t *ctl = NULL;
	pid_t pid = -1;

	ck_assert_int_eq(test_ctl_open(&pid, &ctl, 0), 0);

	snd_ctl_elem_id_t *elem;
	snd_ctl_elem_id_alloca(&elem);
	/* 12:34:56:78:9A:BC - A2DP Playback Volume */
	snd_ctl_elem_id_set_numid(elem, 4);

	long min, max;
	ck_assert_int_eq(snd_ctl_get_dB_range(ctl, elem, &min, &max), 0);
	ck_assert_int_eq(min, -9600);
	ck_assert_int_eq(max, 0);

	ck_assert_int_eq(test_pcm_close(pid, ctl), 0);

} END_TEST

START_TEST(test_single_device) {

	snd_ctl_t *ctl = NULL;
	pid_t pid = -1;

	const char *service = "test";
	ck_assert_int_ne(pid = spawn_bluealsa_server(service, 1,
				true, false, true, true, false, false), -1);

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

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 0), "A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 1), "A2DP Capture Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 2), "A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 3), "A2DP Playback Volume");

	ck_assert_int_eq(test_pcm_close(pid, ctl), 0);

} END_TEST

START_TEST(test_single_device_not_connected) {

	snd_ctl_t *ctl = NULL;
	pid_t pid = -1;

	const char *service = "test";
	ck_assert_int_ne(pid = spawn_bluealsa_server(service, 1,
				true, false, false, false, false, false), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"device \"00:00:00:00:00:00\"", 0), -ENODEV);

	ck_assert_int_eq(test_pcm_close(pid, ctl), 0);

} END_TEST

START_TEST(test_single_device_no_such_device) {

	snd_ctl_t *ctl = NULL;
	pid_t pid = -1;

	const char *service = "test";
	ck_assert_int_ne(pid = spawn_bluealsa_server(service, 1,
				true, false, true, false, false, false), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service,
				"device \"DE:AD:12:34:56:78\"", 0), -ENODEV);

	ck_assert_int_eq(test_pcm_close(pid, ctl), 0);

} END_TEST

START_TEST(test_notifications) {

	snd_ctl_t *ctl = NULL;
	pid_t pid = -1;

	const char *service = "test";
	ck_assert_int_ne(pid = spawn_bluealsa_server(service, -1,
				false, true, true, false, true, false), -1);

	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service, "", 0), 0);
	ck_assert_int_eq(snd_ctl_subscribe_events(ctl, 1), 0);

	snd_ctl_event_t *event;
	snd_ctl_event_malloc(&event);

	size_t events = 0;
	while (snd_ctl_wait(ctl, 500) == 1) {
		ck_assert_int_eq(snd_ctl_read(ctl, event), 1);
		ck_assert_int_eq(snd_ctl_event_get_type(event), SND_CTL_EVENT_ELEM);
		events++;
	}

#if 0
	ck_assert_int_eq(events, 8);
#endif

	snd_ctl_event_free(event);
	ck_assert_int_eq(test_pcm_close(pid, ctl), 0);

} END_TEST

int main(int argc, char *argv[]) {

	preload(argc, argv, ".libs/aloader.so");

	/* test-alsa-ctl and bluealsa-mock shall
	 * be placed in the same directory */
	char *argv_0 = strdup(argv[0]);
	bluealsa_mock_path = dirname(argv_0);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_controls);
	tcase_add_test(tc, test_mute_and_volume);
	tcase_add_test(tc, test_volume_db_range);
	tcase_add_test(tc, test_single_device);
	tcase_add_test(tc, test_single_device_not_connected);
	tcase_add_test(tc, test_single_device_no_such_device);
	tcase_add_test(tc, test_notifications);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	free(argv_0);

	return nf == 0 ? 0 : 1;
}
