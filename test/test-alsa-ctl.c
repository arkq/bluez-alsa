/*
 * test-alsa-ctl.c
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

#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include <check.h>
#include <alsa/asoundlib.h>

#include "inc/preload.inc"
#include "inc/server.inc"

static int snd_ctl_open_bluealsa(snd_ctl_t **ctlp, const char *service, int mode) {

	char buffer[256];
	snd_config_t *conf = NULL;
	snd_input_t *input = NULL;
	int err;

	sprintf(buffer,
			"ctl.bluealsa {\n"
			"  type bluealsa\n"
			"  service \"org.bluealsa.%s\"\n"
			"}\n", service);

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
	if ((*pid = spawn_bluealsa_server(service, 1, true, false, true, true)) == -1)
		return -1;
	return snd_ctl_open_bluealsa(ctl, service, mode);
}

static int test_pcm_close(pid_t pid, snd_ctl_t *ctl) {
	int rv = snd_ctl_close(ctl);
	if (pid != -1) {
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
	}
	return rv;
}

START_TEST(test_control) {

	snd_ctl_t *ctl = NULL;
	pid_t pid = -1;

	ck_assert_int_eq(test_ctl_open(&pid, &ctl, 0), 0);

	snd_ctl_elem_list_t *elems;
	snd_ctl_elem_list_alloca(&elems);

	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);
	ck_assert_int_eq(snd_ctl_elem_list_get_count(elems), 8);
	ck_assert_int_eq(snd_ctl_elem_list_alloc_space(elems, 8), 0);
	ck_assert_int_eq(snd_ctl_elem_list(ctl, elems), 0);

	ck_assert_int_eq(snd_ctl_elem_list_get_used(elems), 8);

	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 0), "12:34:56:78:9A:BC - A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 1), "12:34:56:78:9A:BC - A2DP Capture Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 2), "12:34:56:78:9A:BC - A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 3), "12:34:56:78:9A:BC - A2DP Playback Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 4), "23:45:67:89:AB:CD - A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 5), "23:45:67:89:AB:CD - A2DP Capture Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 6), "23:45:67:89:AB:CD - A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 7), "23:45:67:89:AB:CD - A2DP Playback Volume");

	ck_assert_int_eq(test_pcm_close(pid, ctl), 0);

} END_TEST

START_TEST(test_db_range) {

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

int main(int argc, char *argv[]) {

	preload(argc, argv, ".libs/aloader.so");

	/* test-alsa-ctl and bluealsa-mock shall be placed in the same directory */
	bluealsa_mock_path = dirname(strdup(argv[0]));

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_control);
	tcase_add_test(tc, test_db_range);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
