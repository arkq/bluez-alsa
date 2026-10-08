/*
 * BlueALSA - test-utils-ctl.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
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
#include <glib.h>

#include "fixtures.h"
#include "shared/spawn.h"

static char bluealsactl_path[256];
static int run_bluealsactl(char * output, size_t size, ...) {

	char * argv[32] = { bluealsactl_path };
	size_t n = 1;

	va_list ap;
	va_start(ap, size);

	char * arg;
	while ((arg = va_arg(ap, char *)) != NULL) {
		argv[n++] = arg;
		argv[n] = NULL;
	}

	va_end(ap);

	struct spawn_process sp;
	if (spawn(&sp, argv, NULL, NULL, SPAWN_FLAG_REDIRECT_STDOUT) == -1)
		return -1;

	size_t len;
	if ((len = fread(output, 1, size, sp.f_out)) == size)
		len--;
	output[len] = '\0';

	int wstatus = 0;
	spawn_close(&sp, &wstatus);
	return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
}

CK_START_TEST(test_help) {

	char output[4096];

	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "-q", "-v", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

} CK_END_TEST

CK_START_TEST(test_ba_service_not_running) {

	char output[4096];

	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"--dbus=test", "status", NULL), EXIT_FAILURE);

} CK_END_TEST

CK_START_TEST(test_status) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-source",
				"--profile=hfp-ag",
				NULL), -1);

	char output[4096];

	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "status", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	/* Check default command. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), NULL), EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "Service: org.bluealsa"));
	ck_assert_ptr_nonnull(strstr(output, "A2DP-source"));
	ck_assert_ptr_nonnull(strstr(output, "HFP-AG"));

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_list_services) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, "test", true, NULL), -1);

	char output[4096];

	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "list-services", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	/* Check service listing. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "list-services", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "org.bluealsa.test"));

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_list_pcms) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, "test", true,
				"--profile=a2dp-sink",
				"--profile=hsp-hs",
				NULL), -1);

	char output[4096];

	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "list-pcms", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	/* Check BlueALSA PCM listing. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "--dbus=test", "-v", "list-pcms", NULL),
			EXIT_SUCCESS);

	ck_assert_ptr_nonnull(strstr(output,
				"/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsnk/source"));
	ck_assert_ptr_nonnull(strstr(output,
				"/org/bluealsa/hci11/dev_23_45_67_89_AB_CD/a2dpsnk/source"));
	ck_assert_ptr_nonnull(strstr(output,
				"/org/bluealsa/hci11/dev_23_45_67_89_AB_CD/hsphs/source"));
	ck_assert_ptr_nonnull(strstr(output,
				"/org/bluealsa/hci11/dev_23_45_67_89_AB_CD/hsphs/sink"));

	/* Check verbose output. */
	ck_assert_ptr_nonnull(strstr(output,
				"Device: /org/bluez/hci11/dev_12_34_56_78_9A_BC"));
	ck_assert_ptr_nonnull(strstr(output,
				"Device: /org/bluez/hci11/dev_23_45_67_89_AB_CD"));

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_info) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-source",
#if ENABLE_OFONO
				"--profile=hfp-ofono",
#endif
				NULL), -1);

	char output[4096];

	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "info", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	/* Check not existing BlueALSA PCM path. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"info", "/org/bluealsa/hci11/dev_FF_FF_FF_FF_FF_FF/a2dpsrc/sink", NULL),
			EXIT_FAILURE);

	/* Check BlueALSA PCM info. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"info", "-v", "-v", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", NULL),
			EXIT_SUCCESS);

	ck_assert_ptr_nonnull(strstr(output,
				"Device: /org/bluez/hci11/dev_12_34_56_78_9A_BC"));
	ck_assert_ptr_nonnull(strstr(output,
				"Transport: A2DP-source"));
	ck_assert_ptr_nonnull(strstr(output,
				"Selected codec:\n\tSBC:211502fa [channels: 2] [rate: 44100]"));
	ck_assert_ptr_nonnull(strstr(output,
				"ChannelMap: FL FR"));

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

	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "codec", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	/* Check BlueALSA PCM codec get/set. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"-v", "codec", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/hfpag/sink", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "Available codecs: CVSD"));

#if !ENABLE_HFP_CODEC_SELECTION
	/* CVSD shall be pre-selected if codec selection is not supported. */
	ck_assert_ptr_nonnull(strstr(output, "Selected codec: CVSD"));
#endif

#if ENABLE_MSBC
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"codec", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/hfpag/sink", "mSBC", NULL),
			EXIT_SUCCESS);

	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"codec", "-vf", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/hfpag/sink", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "Selected codec: mSBC"));
#endif

	/* Check selecting not available codec. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"codec", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/hfpag/sink", "SBC", NULL),
			EXIT_FAILURE);

	/* Check selecting A2DP codec (with our mock BlueZ). */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"codec", "-vf", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink",
				"SBC:FF150255", "--channels=1", "--rate=44100", NULL),
			EXIT_SUCCESS);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_client_delay) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-sink",
				NULL), -1);

	char output[4096];

	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "client-delay", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	/* Check default client delay. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"client-delay", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsnk/source", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "ClientDelay: 0.0 ms"));

	/* Check setting client delay. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"client-delay", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsnk/source", "-7.5", NULL),
			EXIT_SUCCESS);

	/* Check that setting client delay does not affect delay. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"info", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsnk/source", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "ClientDelay: -7.5 ms"));
	ck_assert_ptr_nonnull(strstr(output, "Delay: 0.0 ms"));

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_volume) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=a2dp-source",
				NULL), -1);

	char output[4096];

	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "mute", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "soft-volume", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "volume", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	/* Check default volume. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"volume", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "Volume: 50 50"));

	/* Check default mute. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"mute", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "Mute: off off"));

	/* Check default soft-volume. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"soft-volume", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "SoftVolume: false"));

	/* Check setting volume. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"volume", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", "5", "5", NULL),
			EXIT_SUCCESS);
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"volume", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "Volume: 5 5"));

	/* Check setting mute. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"mute", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", "off", "on", NULL),
			EXIT_SUCCESS);
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"mute", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "Mute: off on"));

	/* Check setting soft-volume. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"soft-volume", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", "on", NULL),
			EXIT_SUCCESS);
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"soft-volume", "/org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "SoftVolume: true"));

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

	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "monitor", "--help", NULL),
			EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	/* Check monitor command. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output),
				"monitor", "-v", "--properties=codec,volume", NULL),
			EXIT_SUCCESS);

	/* notifications for service start/stop */
	ck_assert_ptr_nonnull(strstr(output, "ServiceRunning org.bluealsa"));
	ck_assert_ptr_nonnull(strstr(output, "ServiceStopped org.bluealsa"));

	/* notifications for PCM add/remove */
	ck_assert_ptr_nonnull(strstr(output,
				"PCMAdded /org/bluealsa/hci11/dev_23_45_67_89_AB_CD/a2dpsrc/sink"));
	ck_assert_ptr_nonnull(strstr(output,
				"PCMRemoved /org/bluealsa/hci11/dev_23_45_67_89_AB_CD/a2dpsrc/sink"));

	/* notifications for RFCOMM add/remove (because HFP is enabled) */
	ck_assert_ptr_nonnull(strstr(output,
				"RFCOMMAdded /org/bluealsa/hci11/dev_12_34_56_78_9A_BC/rfcomm"));
	ck_assert_ptr_nonnull(strstr(output,
				"RFCOMMRemoved /org/bluealsa/hci11/dev_12_34_56_78_9A_BC/rfcomm"));

	/* check verbose output */
	ck_assert_ptr_nonnull(strstr(output,
				"Device: /org/bluez/hci11/dev_12_34_56_78_9A_BC"));
	ck_assert_ptr_nonnull(strstr(output,
				"Device: /org/bluez/hci11/dev_23_45_67_89_AB_CD"));

	/* notifications for property changed */
	ck_assert_ptr_nonnull(strstr(output,
				"PropertyChanged /org/bluealsa/hci11/dev_12_34_56_78_9A_BC/a2dpsrc/sink Volume 54 54"));
	ck_assert_ptr_nonnull(strstr(output,
				"PropertyChanged /org/bluealsa/hci11/dev_23_45_67_89_AB_CD/a2dpsrc/sink Volume 84 84"));
#if ENABLE_MSBC
	ck_assert_ptr_nonnull(strstr(output,
				"PropertyChanged /org/bluealsa/hci11/dev_12_34_56_78_9A_BC/hfpag/sink Codec CVSD"));
	ck_assert_ptr_nonnull(strstr(output,
				"PropertyChanged /org/bluealsa/hci11/dev_12_34_56_78_9A_BC/hfpag/source Codec CVSD"));
#endif

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

CK_START_TEST(test_open) {

	struct spawn_process sp_ba_mock;
	ck_assert_int_ne(spawn_bluealsa_mock(&sp_ba_mock, NULL, true,
				"--profile=hsp-ag",
				NULL), -1);

	char output[4096];
	/* Check printing help text. */
	ck_assert_int_eq(run_bluealsactl(output, sizeof(output), "open", "--help", NULL), EXIT_SUCCESS);
	ck_assert_ptr_nonnull(strstr(output, "-h, --help"));

	char * bactl_in_argv[32] = {
		bluealsactl_path, "open", "--hex",
		"/org/bluealsa/hci11/dev_23_45_67_89_AB_CD/hspag/source",
		NULL };
	char * bactl_out_argv[32] = {
		bluealsactl_path, "open", "--hex",
		"/org/bluealsa/hci11/dev_23_45_67_89_AB_CD/hspag/sink",
		NULL };

	struct spawn_process sp_bactl_in;
	ck_assert_int_ne(spawn(&sp_bactl_in, bactl_in_argv, NULL,
				NULL, SPAWN_FLAG_REDIRECT_STDOUT), -1);

	struct spawn_process sp_bactl_out;
	ck_assert_int_ne(spawn(&sp_bactl_out, bactl_out_argv, NULL,
				sp_bactl_in.f_stdout, SPAWN_FLAG_NONE), -1);

	/* let it run for a while */
	usleep(250000);

	spawn_terminate(&sp_bactl_in, 0);
	spawn_terminate(&sp_bactl_out, 500);

	int wstatus = 0;
	/* Make sure that input bluealsactl instances have been terminated by
	 * us (SIGTERM) and not by premature exit or any other reason. On the other
	 * hand, the output bluealsactl instance should exit gracefully because
	 * of the end of input stream. */
	spawn_close(&sp_bactl_in, &wstatus);
	ck_assert_int_eq(WTERMSIG(wstatus), SIGTERM);
	spawn_close(&sp_bactl_out, &wstatus);
	ck_assert_int_eq(WIFEXITED(wstatus), 1);
	ck_assert_int_eq(WEXITSTATUS(wstatus), 0);

	spawn_terminate(&sp_ba_mock, 0);
	spawn_close(&sp_ba_mock, NULL);

} CK_END_TEST

int main(int argc, char *argv[]) {
	preload(argc, argv, ".libs/libalsaloader.so");

	g_autofree char * argv_0 = strdup(argv[0]);
	const char * argv_0_dir = dirname(argv_0);

	snprintf(bluealsad_mock_path, sizeof(bluealsad_mock_path),
			"%s/mock/bluealsad-mock", argv_0_dir);
	snprintf(bluealsactl_path, sizeof(bluealsactl_path),
			"%s/../src/bluealsactl/bluealsactl", argv_0_dir);

	Suite * s = suite_create(__FILE__);
	TCase * tc = tcase_create(__FILE__);
	g_autoptr(SRunner) sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_help);
	tcase_add_test(tc, test_ba_service_not_running);
	tcase_add_test(tc, test_status);
	tcase_add_test(tc, test_list_services);
	tcase_add_test(tc, test_list_pcms);
	tcase_add_test(tc, test_info);
	tcase_add_test(tc, test_codec);
	tcase_add_test(tc, test_client_delay);
	tcase_add_test(tc, test_volume);
	tcase_add_test(tc, test_monitor);
	tcase_add_test(tc, test_open);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	return nf == 0 ? 0 : 1;
}
