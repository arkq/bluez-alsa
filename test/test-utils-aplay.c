/*
 * test-utils-aplay.c
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

#include <libgen.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "inc/check.inc"
#include "inc/mock.inc"
#include "inc/preload.inc"
#include "inc/spawn.inc"

static char bluealsa_aplay_path[256];
static int spawn_bluealsa_aplay(struct spawn_process *sp, ...) {

	char * argv[32] = { bluealsa_aplay_path };
	size_t n = 1;

	va_list ap;
	va_start(ap, sp);

	char *arg;
	while ((arg = va_arg(ap, char *)) != NULL) {
		argv[n++] = arg;
		argv[n] = NULL;
	}

	va_end(ap);

	const int flags = SPAWN_FLAG_REDIRECT_STDOUT | SPAWN_FLAG_REDIRECT_STDERR;
	return spawn(sp, argv, NULL, flags);
}

CK_START_TEST(test_help) {

	struct spawn_process sp_ba_aplay;
	ck_assert_int_ne(spawn_bluealsa_aplay(&sp_ba_aplay,
				"-v", "--help", NULL), -1);

	char output[4096] = "";
	ck_assert_int_gt(fread(output, 1, sizeof(output) - 1, sp_ba_aplay.f_stdout), 0);
	fprintf(stderr, "%s", output);

	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	spawn_close(&sp_ba_aplay, NULL);

} CK_END_TEST

CK_START_TEST(test_configuration) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, "foo", true,
				NULL), -1);

	struct spawn_process sp_ba_aplay;
	ck_assert_int_ne(spawn_bluealsa_aplay(&sp_ba_aplay,
				"--verbose",
				"--dbus=foo",
				"--pcm=TestPCM",
				"--pcm-buffer-time=10000",
				"--pcm-period-time=500",
				"--mixer-device=TestMixer",
				"--mixer-name=TestMixerName",
				"--mixer-index=1",
				"--profile-sco",
				"12:34:56:78:90:AB",
				NULL), -1);
	spawn_terminate(&sp_ba_aplay, 100);

	char output[4096] = "";
	ck_assert_int_gt(fread(output, 1, sizeof(output) - 1, sp_ba_aplay.f_stderr), 0);
	fprintf(stderr, "%s", output);

	/* check selected configuration */
	ck_assert_ptr_ne(strstr(output, "  BlueALSA service: org.bluealsa.foo"), NULL);
	ck_assert_ptr_ne(strstr(output, "  ALSA PCM device: TestPCM"), NULL);
	ck_assert_ptr_ne(strstr(output, "  ALSA PCM buffer time: 10000 us"), NULL);
	ck_assert_ptr_ne(strstr(output, "  ALSA PCM period time: 500 us"), NULL);
	ck_assert_ptr_ne(strstr(output, "  ALSA mixer device: TestMixer"), NULL);
	ck_assert_ptr_ne(strstr(output, "  ALSA mixer element: 'TestMixerName',1"), NULL);
	ck_assert_ptr_ne(strstr(output, "  Bluetooth device(s): 12:34:56:78:90:AB"), NULL);
	ck_assert_ptr_ne(strstr(output, "  Profile: SCO"), NULL);

	spawn_close(&sp_ba_aplay, NULL);
	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_list_devices) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--device-name=23:45:67:89:AB:CD:Speaker",
				"--profile=a2dp-source",
				"--profile=hsp-ag",
				NULL), -1);

	struct spawn_process sp_ba_aplay;
	ck_assert_int_ne(spawn_bluealsa_aplay(&sp_ba_aplay,
				"--list-devices",
				NULL), -1);

	char output[4096] = "";
	ck_assert_int_gt(fread(output, 1, sizeof(output) - 1, sp_ba_aplay.f_stdout), 0);
	fprintf(stderr, "%s", output);

	ck_assert_ptr_ne(strstr(output,
				"hci0: 23:45:67:89:AB:CD [Speaker], audio-card"), NULL);

	spawn_close(&sp_ba_aplay, NULL);
	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_list_pcms) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, "foo", true,
				"--device-name=23:45:67:89:AB:CD:Speaker",
				"--profile=a2dp-source",
				"--profile=hsp-ag",
				NULL), -1);

	struct spawn_process sp_ba_aplay;
	ck_assert_int_ne(spawn_bluealsa_aplay(&sp_ba_aplay,
				"--dbus=foo",
				"--list-pcms",
				NULL), -1);

	char output[4096] = "";
	ck_assert_int_gt(fread(output, 1, sizeof(output) - 1, sp_ba_aplay.f_stdout), 0);
	fprintf(stderr, "%s", output);

	ck_assert_ptr_ne(strstr(output,
				"bluealsa:DEV=23:45:67:89:AB:CD,PROFILE=sco,SRV=org.bluealsa.foo"), NULL);

	spawn_close(&sp_ba_aplay, NULL);
	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_play_all) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-sink",
				NULL), -1);

	struct spawn_process sp_ba_aplay;
	ck_assert_int_ne(spawn_bluealsa_aplay(&sp_ba_aplay,
				"--profile-a2dp",
				"--pcm=null",
				"-v", "-v",
				NULL), -1);
	spawn_terminate(&sp_ba_aplay, 500);

	char output[8192] = "";
	ck_assert_int_gt(fread(output, 1, sizeof(output) - 1, sp_ba_aplay.f_stderr), 0);
	fprintf(stderr, "%s", output);

	/* check if playback was started from both devices */
	ck_assert_ptr_ne(strstr(output,
				"Used configuration for 12:34:56:78:9A:BC"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Used configuration for 23:45:67:89:AB:CD"), NULL);

	spawn_close(&sp_ba_aplay, NULL);
	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_play_single_audio) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-sink",
				NULL), -1);

	struct spawn_process sp_ba_aplay;
	ck_assert_int_ne(spawn_bluealsa_aplay(&sp_ba_aplay,
				"--single-audio",
				"--profile-a2dp",
				"--pcm=null",
				"-v", "-v", "-v",
				NULL), -1);
	spawn_terminate(&sp_ba_aplay, 500);

	char output[8192] = "";
	ck_assert_int_gt(fread(output, 1, sizeof(output) - 1, sp_ba_aplay.f_stderr), 0);
	fprintf(stderr, "%s", output);

	/* Check if playback was started for only one device. However,
	 * workers should be created for both devices. */

#if DEBUG
	ck_assert_ptr_ne(strstr(output,
				"Creating IO worker 12:34:56:78:9A:BC"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Creating IO worker 23:45:67:89:AB:CD"), NULL);
#endif

	bool d1_ok = strstr(output, "Used configuration for 12:34:56:78:9A:BC") != NULL;
	bool d2_ok = strstr(output, "Used configuration for 23:45:67:89:AB:CD") != NULL;
	ck_assert_int_eq(d1_ok != d2_ok, true);

	spawn_close(&sp_ba_aplay, NULL);
	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_play_mixer_setup) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--device-name=23:45:67:89:AB:CD:Headset",
				"--profile=hsp-ag",
				NULL), -1);

	struct spawn_process sp_ba_aplay;
	ck_assert_int_ne(spawn_bluealsa_aplay(&sp_ba_aplay,
				"--profile-sco",
				"--pcm=bluealsa:PROFILE=sco",
				"--volume=mixer",
				"--mixer-device=bluealsa:DEV=23:45:67:89:AB:CD",
				"--mixer-name=SCO",
				"-v",
				NULL), -1);
	spawn_terminate(&sp_ba_aplay, 500);

	char output[8192] = "";
	ck_assert_int_gt(fread(output, 1, sizeof(output) - 1, sp_ba_aplay.f_stderr), 0);
	fprintf(stderr, "%s", output);

#if DEBUG
	ck_assert_ptr_ne(strstr(output,
				"Opening ALSA mixer: name=bluealsa:DEV=23:45:67:89:AB:CD elem=SCO index=0"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Setting up ALSA mixer volume synchronization"), NULL);
#endif

	spawn_close(&sp_ba_aplay, NULL);
	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_play_dbus_signals) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, false,
				"--timeout=0",
				"--profile=hfp-ag",
				"--fuzzing=250",
				NULL), -1);

	struct spawn_process sp_ba_aplay;
	ck_assert_int_ne(spawn_bluealsa_aplay(&sp_ba_aplay,
				"--profile-sco",
				"--pcm=null",
				"-v", "-v",
				NULL), -1);
	spawn_terminate(&sp_ba_aplay, 1500);

	char output[8192] = "";
	ck_assert_int_gt(fread(output, 1, sizeof(output) - 1, sp_ba_aplay.f_stderr), 0);
	fprintf(stderr, "%s", output);

#if ENABLE_MSBC && DEBUG
	/* with mSBC support, codec is not selected right away */
	ck_assert_ptr_ne(strstr(output,
				"Skipping SCO with codec not selected"), NULL);
#endif

	ck_assert_ptr_ne(strstr(output,
				"Used configuration for 12:34:56:78:9A:BC"), NULL);
	/* check proper sampling rate for CVSD codec */
	ck_assert_ptr_ne(strstr(output,
				"Sampling rate: 8000 Hz"), NULL);

#if ENABLE_MSBC
	ck_assert_ptr_ne(strstr(output,
				"Used configuration for 12:34:56:78:9A:BC"), NULL);
	/* check proper sampling rate for mSBC codec */
	ck_assert_ptr_ne(strstr(output,
				"Sampling rate: 16000 Hz"), NULL);
#endif

	spawn_close(&sp_ba_aplay, NULL);
	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

int main(int argc, char *argv[], char *envp[]) {
	preload(argc, argv, envp, ".libs/aloader.so");

	char *argv_0 = strdup(argv[0]);
	char *argv_0_dir = dirname(argv_0);

	snprintf(bluealsa_mock_path, sizeof(bluealsa_mock_path),
			"%s/mock/bluealsa-mock", argv_0_dir);
	snprintf(bluealsa_aplay_path, sizeof(bluealsa_aplay_path),
			"%s/../utils/aplay/bluealsa-aplay", argv_0_dir);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_help);
	tcase_add_test(tc, test_configuration);
	tcase_add_test(tc, test_list_devices);
	tcase_add_test(tc, test_list_pcms);
	tcase_add_test(tc, test_play_all);
	tcase_add_test(tc, test_play_single_audio);
	tcase_add_test(tc, test_play_mixer_setup);
	tcase_add_test(tc, test_play_dbus_signals);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	free(argv_0);

	return nf == 0 ? 0 : 1;
}
