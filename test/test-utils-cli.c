/*
 * test-utils-cli.c
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
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include <check.h>

#include "inc/preload.inc"
#include "inc/server.inc"

static char bluealsa_cli_path[256];
static int run_bluealsa_cli(const char *arguments, char *output, size_t size) {

	FILE *f;
	char command[1024];

	snprintf(command, sizeof(command), "%s %s", bluealsa_cli_path,
			arguments != NULL ? arguments : "");
	ck_assert_ptr_ne(f = popen(command, "r"), NULL);

	size_t len = fread(output, 1, size - 1, f);
	output[len] = '\0';

	fprintf(stderr, "%s", output);

	return pclose(f);
}

START_TEST(test_status) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	pid_t pid;
	ck_assert_int_ne(pid = spawn_bluealsa_server(NULL, true,
				"--profile=a2dp-source",
				"--profile=hfp-ag",
				NULL), -1);

	char output[512];
	const char *args;

	/* check printing help text */
	args = "-qv --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check printing help text */
	args = "status --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check default command */
	ck_assert_int_eq(run_bluealsa_cli(NULL, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "Service: org.bluealsa"), NULL);
	ck_assert_ptr_ne(strstr(output, "A2DP-source"), NULL);
	ck_assert_ptr_ne(strstr(output, "HFP-AG"), NULL);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_list_services) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	pid_t pid;
	ck_assert_int_ne(pid = spawn_bluealsa_server("test", true, NULL), -1);

	char output[512];
	const char *args;

	/* check printing help text */
	args = "list-services --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check service listing */
	args = "list-services";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "org.bluealsa.test"), NULL);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_list_pcms) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	pid_t pid;
	ck_assert_int_ne(pid = spawn_bluealsa_server("test", true,
				"--profile=a2dp-sink",
				"--profile=hsp-ag",
				NULL), -1);

	char output[2048];
	const char *args;

	/* check printing help text */
	args = "list-pcms --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check BlueALSA PCM listing */
	args = "--dbus=test --verbose list-pcms";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);

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

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_info) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	pid_t pid;
	ck_assert_int_ne(pid = spawn_bluealsa_server(NULL, true,
				"--profile=a2dp-source",
				NULL), -1);

	char output[512];
	const char *args;

	/* check printing help text */
	args = "info --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check BlueALSA PCM info */
	args = "info /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);

	ck_assert_ptr_ne(strstr(output,
				"Device: /org/bluez/hci0/dev_12_34_56_78_9A_BC"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Transport: A2DP-source"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"Selected codec: SBC"), NULL);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_codec) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	pid_t pid;
	ck_assert_int_ne(pid = spawn_bluealsa_server(NULL, true,
				"--profile=hfp-ag",
				NULL), -1);

	char output[512];
	const char *args;

	/* check printing help text */
	args = "codec --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check BlueALSA PCM codec get/set */
	args = "-v codec /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "Available codecs: CVSD"), NULL);

#if !ENABLE_MSBC
	/* CVSD shall be automatically selected if mSBC is not supported. */
	ck_assert_ptr_ne(strstr(output, "Selected codec: CVSD"), NULL);
#endif

#if ENABLE_MSBC
	args = "codec /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink mSBC";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);

	args = "-v codec /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "Selected codec: mSBC"), NULL);
#endif

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_volume) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	pid_t pid;
	ck_assert_int_ne(pid = spawn_bluealsa_server(NULL, true,
				"--profile=a2dp-source",
				NULL), -1);

	char output[512];
	const char *args;

	/* check printing help text */
	args = "mute --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);
	args = "soft-volume --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);
	args = "volume --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check default volume */
	args = "volume /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "Volume: L: 127 R: 127"), NULL);

	/* check default mute */
	args = "mute /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "Muted: L: false R: false"), NULL);

	/* check default soft-volume */
	args = "soft-volume /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "SoftVolume: true"), NULL);

	/* check setting volume */
	args = "volume /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink 10 50";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	args = "volume /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "Volume: L: 10 R: 50"), NULL);

	/* check setting mute */
	args = "mute /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink off on";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	args = "mute /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "Muted: L: false R: true"), NULL);

	/* check setting soft-volume */
	args = "soft-volume /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink off";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	args = "soft-volume /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/a2dpsrc/sink";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "SoftVolume: false"), NULL);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_monitor) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	pid_t pid;
	ck_assert_int_ne(pid = spawn_bluealsa_server(NULL, false,
				"--timeout=0",
				"--fuzzing=200",
				"--profile=a2dp-source",
				"--profile=hfp-ag",
				NULL), -1);

	char output[2048];
	const char *args;

	/* check printing help text */
	args = "monitor --help";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);
	ck_assert_ptr_ne(strstr(output, "-h, --help"), NULL);

	/* check monitor command */
	args = "-v monitor --properties=codec,volume";
	ck_assert_int_eq(run_bluealsa_cli(args, output, sizeof(output)), 0);

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

	/* notifications for property changed */
	ck_assert_ptr_ne(strstr(output,
				"PropertyChanged /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/sink Codec CVSD"), NULL);
	ck_assert_ptr_ne(strstr(output,
				"PropertyChanged /org/bluealsa/hci0/dev_12_34_56_78_9A_BC/hfpag/source Codec CVSD"), NULL);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

} END_TEST

int main(int argc, char *argv[], char *envp[]) {
	preload(argc, argv, envp, ".libs/aloader.so");

	char *argv_0 = strdup(argv[0]);
	char *argv_0_dir = dirname(argv_0);

	snprintf(bluealsa_mock_path, sizeof(bluealsa_mock_path),
			"%s/bluealsa-mock", argv_0_dir);
	snprintf(bluealsa_cli_path, sizeof(bluealsa_cli_path),
			"%s/../utils/cli/bluealsa-cli", argv_0_dir);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_status);
	tcase_add_test(tc, test_list_services);
	tcase_add_test(tc, test_list_pcms);
	tcase_add_test(tc, test_info);
	tcase_add_test(tc, test_codec);
	tcase_add_test(tc, test_volume);
	tcase_add_test(tc, test_monitor);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	free(argv_0);

	return nf == 0 ? 0 : 1;
}
