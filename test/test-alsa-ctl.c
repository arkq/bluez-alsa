/*
 * test-alsa-ctl.c
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

#include <libgen.h>
#include <sys/wait.h>

#include <check.h>
#include <alsa/asoundlib.h>

#include "inc/preload.inc"
#include "inc/server.inc"
#include "../src/shared/log.c"

static int snd_ctl_open_bluealsa(snd_ctl_t **ctlp, const char *service, int mode) {

	char buffer[256];
	snd_config_t *conf = NULL;
	snd_input_t *input = NULL;
	int err;

	sprintf(buffer,
			"ctl.bluealsa {\n"
			"  type bluealsa\n"
			"  service \"%s\"\n"
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

START_TEST(test_control) {

	const char *service = "org.bluealsa.test";
	pid_t pid = spawn_bluealsa_server(service, 1, false, true, true);

	snd_ctl_t *ctl = NULL;
	ck_assert_int_eq(snd_ctl_open_bluealsa(&ctl, service, 0), 0);

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
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 4), "12:34:56:9A:BC:DE - A2DP Capture Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 5), "12:34:56:9A:BC:DE - A2DP Capture Volume");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 6), "12:34:56:9A:BC:DE - A2DP Playback Switch");
	ck_assert_str_eq(snd_ctl_elem_list_get_name(elems, 7), "12:34:56:9A:BC:DE - A2DP Playback Volume");

	ck_assert_int_eq(snd_ctl_close(ctl), 0);

	waitpid(pid, NULL, 0);

} END_TEST

int main(int argc, char *argv[]) {

	preload(argc, argv, ".libs/aloader.so");

	/* test-alsa-ctl and server-mock shall be placed in the same directory */
	bin_path = dirname(argv[0]);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_control);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
