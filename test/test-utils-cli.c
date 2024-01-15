/*
 * test-utils-cli.c
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

#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <check.h>

#include "inc/check.inc"
#include "inc/mock.inc"
#include "inc/preload.inc"
#include "inc/spawn.inc"

static char bluealsa_cli_path[256];
static int run_bluealsa_cli(char *output, size_t size, ...) {

	char * argv[32] = { bluealsa_cli_path };
	size_t n = 1;

	va_list ap;
	va_start(ap, size);

	char *arg;
	while ((arg = va_arg(ap, char *)) != NULL) {
		argv[n++] = arg;
		argv[n] = NULL;
	}

	va_end(ap);

	struct spawn_process sp;
	if (spawn(&sp, argv, NULL, SPAWN_FLAG_REDIRECT_STDOUT) == -1)
		return -1;

	size_t len = fread(output, 1, size - 1, sp.f_stdout);
	output[len] = '\0';

	fprintf(stderr, "%s", output);

	int wstatus = 0;
	spawn_close(&sp, &wstatus);
	return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
}

CK_START_TEST(test_help) {

	char output[4096];

	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"-q", "-v", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

} CK_END_TEST

CK_START_TEST(test_status) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-source",
				"--profile=hfp-ag",
				NULL), -1);

	char output[4096];

	/* check printing help text */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"status", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check default command */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "Service: org.bluealsa"), NULL);
	ck_assert_ptr_ne(strstr(output, "A2DP-source"), NULL);
	ck_assert_ptr_ne(strstr(output, "HFP-AG"), NULL);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_list_services) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, "test", true, NULL), -1);

	char output[4096];

	/* check printing help text */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"list-services", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check service listing */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"list-services",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "org.bluealsa.test"), NULL);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_list_pcms) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, "test", true,
				"--profile=a2dp-sink",
				"--profile=hsp-ag",
				NULL), -1);

	char output[4096];

	/* check printing help text */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"list-pcms", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check BlueALSA PCM listing */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"--dbus=test", "--verbose", "list-pcms",
				NULL), 0);

	ck_assert_ptr_ne(strstr(output,
				"/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsnk/source"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"/org/bluealsa/hci0/dev_23_45_67_89_AB_CD/hspag/source"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"/org/bluealsa/hci0/dev_23_45_67_89_AB_CD/hspag/sink"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"/org/bluealsa/hci0/dev_23_45_67_89_AB_CD/a2dpsnk/source"), NULL);

	/* check verbose output */
	ck_assert_ptr_ne(strstr(output,
				"Device: /org/bluez/hci0/dev_12_34_56_78_9A_BC"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Device: /org/bluez/hci0/dev_23_45_67_89_AB_CD"), NULL);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_info) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-source",
				NULL), -1);

	char output[4096];

	/* check printing help text */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"info", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check not existing BlueALSA PCM path */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"info", "/org/bluealsa/hci0/dev_FF_FF_FF_FF_FF_FF/a2dpsrc/sink",
				NULL), EXIT_FAILURE);

	/* check BlueALSA PCM info */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"info", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);

	ck_assert_ptr_ne(strstr(output,
				"Device: /org/bluez/hci0/dev_12_34_56_78_9A_BC"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Transport: A2DP-source"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Selected codec: SBC"), NULL);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_codec) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-source",
				"--profile=hfp-ag",
				NULL), -1);

	char output[4096];

	/* check printing help text */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"codec", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check BlueALSA PCM codec get/set */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"-v", "codec", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "Available codecs: CVSD"), NULL);

#if !ENABLE_MSBC
	/* CVSD shall be automatically selected if mSBC is not supported. */
	ck_assert_ptr_ne(strstr(output, "Selected codec: CVSD"), NULL);
#endif

#if ENABLE_MSBC
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"codec", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink", "mSBC",
				NULL), 0);

	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"codec", "-vf", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "Selected codec: mSBC"), NULL);
#endif

	/* check selecting not available codec */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"codec", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink", "SBC",
				NULL), EXIT_FAILURE);

	/* check selecting A2DP codec without SEP support (with our mock BlueZ) */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"codec", "-vf", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				"SBC", "11150255",
				NULL), EXIT_FAILURE);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_delay_adjustment) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-source",
				NULL), -1);

	char output[4096];

	/* check printing help text */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"delay-adjustment", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check default delay adjustment */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"delay-adjustment", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "DelayAdjustment: 0.0 ms"), NULL);

	/* check setting delay adjustment */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"delay-adjustment", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink", "-7.5",
				NULL), 0);
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"delay-adjustment", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "DelayAdjustment: -7.5 ms"), NULL);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_volume) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-source",
				NULL), -1);

	char output[4096];

	/* check printing help text */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"mute", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"soft-volume", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"volume", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check default volume */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"volume", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "Volume: L: 127 R: 127"), NULL);

	/* check default mute */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"mute", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "Muted: L: false R: false"), NULL);

	/* check default soft-volume */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"soft-volume", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "SoftVolume: true"), NULL);

	/* check setting volume */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"volume", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink", "10", "50",
				NULL), 0);
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"volume", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "Volume: L: 10 R: 50"), NULL);

	/* check setting mute */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"mute", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink", "off", "on",
				NULL), 0);
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"mute", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "Muted: L: false R: true"), NULL);

	/* check setting soft-volume */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"soft-volume", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink", "off",
				NULL), 0);
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"soft-volume", "/org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				NULL), 0);
	ck_assert_ptr_ne(strstr(output, "SoftVolume: false"), NULL);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_monitor) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, false,
				"--timeout=0",
				"--fuzzing=200",
				"--profile=a2dp-source",
				"--profile=hfp-ag",
				NULL), -1);

	char output[4096];

	/* check printing help text */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"monitor", "--help", NULL), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check monitor command */
	ck_assert_int_eq(run_bluealsa_cli(output, sizeof(output),
				"monitor", "-v", "--properties=codec,volume",
				NULL), 0);

	/* notifications for service start/stop */
	ck_assert_ptr_ne(strstr(output, "ServiceRunning org.bluealsa"), NULL);
	ck_assert_ptr_ne(strstr(output, "ServiceStopped org.bluealsa"), NULL);

	/* notifications for PCM add/remove */
	ck_assert_ptr_ne(strstr(output,
				"PCMAdded /org/bluealsa/hci0/dev_23_45_67_89_AB_CD/a2dpsrc/sink"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"PCMRemoved /org/bluealsa/hci0/dev_23_45_67_89_AB_CD/a2dpsrc/sink"), NULL);

	/* notifications for RFCOMM add/remove (because HFP is enabled) */
	ck_assert_ptr_ne(strstr(output,
				"RFCOMMAdded /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/rfcomm"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"RFCOMMRemoved /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/rfcomm"), NULL);

	/* check verbose output */
	ck_assert_ptr_ne(strstr(output,
				"Device: /org/bluez/hci0/dev_12_34_56_78_9A_BC"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Device: /org/bluez/hci0/dev_23_45_67_89_AB_CD"), NULL);

#if ENABLE_MSBC
	/* notifications for property changed */
	ck_assert_ptr_ne(strstr(output,
				"PropertyChanged /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink Codec CVSD"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"PropertyChanged /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/source Codec CVSD"), NULL);
#endif

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_open) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=hsp-ag",
				NULL), -1);

	char * ba_cli_in_argv[32] = {
		bluealsa_cli_path, "open", "--hex",
		"/org/bluealsa/hci0/dev_23_45_67_89_AB_CD/hspag/source",
		NULL };
	char * ba_cli_out_argv[32] = {
		bluealsa_cli_path, "open", "--hex",
		"/org/bluealsa/hci0/dev_23_45_67_89_AB_CD/hspag/sink",
		NULL };

	struct spawn_process sp_ba_cli_in;
	ck_assert_int_ne(spawn(&sp_ba_cli_in, ba_cli_in_argv,
				NULL, SPAWN_FLAG_REDIRECT_STDOUT), -1);

	struct spawn_process sp_ba_cli_out;
	ck_assert_int_ne(spawn(&sp_ba_cli_out, ba_cli_out_argv,
				sp_ba_cli_in.f_stdout, SPAWN_FLAG_NONE), -1);

	/* let it run for a while */
	usleep(250000);

	spawn_terminate(&sp_ba_cli_in, 0);
	spawn_terminate(&sp_ba_cli_out, 500);

	int wstatus = 0;
	/* Make sure that input bluealsa-cli instances have been terminated by
	 * us (SIGTERM) and not by premature exit or any other reason. On the other
	 * hand, the output bluealsa-cli instance should exit gracefully because
	 * of the end of input stream. */
	spawn_close(&sp_ba_cli_in, &wstatus);
	ck_assert_int_eq(WTERMSIG(wstatus), SIGTERM);
	spawn_close(&sp_ba_cli_out, &wstatus);
	ck_assert_int_eq(WIFEXITED(wstatus), 1);
	ck_assert_int_eq(WEXITSTATUS(wstatus), 0);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

int main(int argc, char *argv[], char *envp[]) {
	preload(argc, argv, envp, ".libs/aloader.so");

	char *argv_0 = strdup(argv[0]);
	char *argv_0_dir = dirname(argv_0);

	snprintf(bluealsa_mock_path, sizeof(bluealsa_mock_path),
			"%s/mock/bluealsa-mock", argv_0_dir);
	snprintf(bluealsa_cli_path, sizeof(bluealsa_cli_path),
			"%s/../utils/cli/bluealsa-cli", argv_0_dir);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_help);
	tcase_add_test(tc, test_status);
	tcase_add_test(tc, test_list_services);
	tcase_add_test(tc, test_list_pcms);
	tcase_add_test(tc, test_info);
	tcase_add_test(tc, test_codec);
	tcase_add_test(tc, test_delay_adjustment);
	tcase_add_test(tc, test_volume);
	tcase_add_test(tc, test_monitor);
	tcase_add_test(tc, test_open);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	free(argv_0);

	return nf == 0 ? 0 : 1;
}
