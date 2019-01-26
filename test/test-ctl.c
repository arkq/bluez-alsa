/*
 * test-ctl.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <libgen.h>
#include <sys/wait.h>

#include <check.h>

#include "inc/server.inc"
#include "../src/shared/ctl-client.c"
#include "../src/shared/log.c"

static bdaddr_t addr0;
static bdaddr_t addr1;

START_TEST(test_open) {

	const char *hci = "hci-tc0";

	ck_assert_int_eq(bluealsa_open(hci), -1);
	ck_assert_int_eq(errno, ENOENT);

	pid_t pid = spawn_bluealsa_server(hci, 1, false, false, false);
	ck_assert_int_ne(bluealsa_open(hci), -1);

	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_subscribe) {

	const char *hci = "hci-tc1";
	pid_t pid = spawn_bluealsa_server(hci, 1, true, true, false);

	int fd = -1;
	ck_assert_int_ne(fd = bluealsa_open(hci), -1);

	ck_assert_int_ne(bluealsa_event_subscribe(fd, BA_EVENT_TRANSPORT_ADDED), -1);

	struct ba_msg_event ev0, ev1;
	ck_assert_int_eq(read(fd, &ev0, sizeof(ev0)), sizeof(ev0));
	ck_assert_int_eq(read(fd, &ev1, sizeof(ev1)), sizeof(ev1));

	struct ba_msg_transport t0, t1;
	ck_assert_int_ne(bluealsa_get_transport(fd, &addr0, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK, &t0), -1);
	ck_assert_int_ne(bluealsa_get_transport(fd, &addr1, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK, &t1), -1);

	ck_assert_int_eq(bluealsa_event_match(&t0, &ev0), 0);
	ck_assert_int_eq(bluealsa_event_match(&t1, &ev1), 0);

	close(fd);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_get_devices) {

	const char *hci = "hci-tc2";
	pid_t pid = spawn_bluealsa_server(hci, 1, false, true, true);

	int fd = -1;
	ck_assert_int_ne(fd = bluealsa_open(hci), -1);

	struct ba_msg_device *devices;
	ck_assert_int_eq(bluealsa_get_devices(fd, &devices), 2);

	ck_assert_int_eq(bacmp(&devices[0].addr, &addr0), 0);
	ck_assert_str_eq(devices[0].name, "Test Device With Long Name");
	ck_assert_int_eq(bacmp(&devices[1].addr, &addr1), 0);
	ck_assert_str_eq(devices[1].name, "Test Device With Long Name");

	struct ba_msg_transport *transports;
	ck_assert_int_eq(bluealsa_get_transports(fd, &transports), 4);

	ck_assert_int_eq(bacmp(&transports[0].addr, &addr0), 0);
	ck_assert_int_eq(transports[0].type, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK);
	ck_assert_int_eq(bacmp(&transports[1].addr, &addr0), 0);
	ck_assert_int_eq(transports[1].type, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_CAPTURE);
	ck_assert_int_eq(bacmp(&transports[2].addr, &addr1), 0);
	ck_assert_int_eq(transports[2].type, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK);
	ck_assert_int_eq(bacmp(&transports[3].addr, &addr1), 0);
	ck_assert_int_eq(transports[3].type, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_CAPTURE);

	ck_assert_int_eq(transports[0].codec, 0);
	ck_assert_int_eq(transports[0].channels, 2);
	ck_assert_int_eq(transports[0].sampling, 44100);
	ck_assert_int_eq(transports[0].ch1_muted, 0);
	ck_assert_int_eq(transports[0].ch1_volume, 127);
	ck_assert_int_eq(transports[0].ch2_muted, 0);
	ck_assert_int_eq(transports[0].ch2_volume, 127);
	ck_assert_int_eq(transports[0].delay, 0);

	close(fd);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_get_transport) {

	const char *hci = "hci-tc3";
	pid_t pid = spawn_bluealsa_server(hci, 1, false, true, false);

	int fd = -1;
	ck_assert_int_ne(fd = bluealsa_open(hci), -1);

	struct ba_msg_transport t;
	ck_assert_int_ne(bluealsa_get_transport(fd, &addr0, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK, &t), -1);

	unsigned int delay = 0;
	ck_assert_int_eq(bluealsa_get_transport_delay(fd, &t, &delay), 0);
	ck_assert_int_eq(delay, 0);

	bool ch1_muted = false, ch2_muted = false;
	int ch1_volume = 0, ch2_volume = 0;
	ck_assert_int_eq(bluealsa_set_transport_volume(fd, &t, true, 15, true, 50), 0);
	ck_assert_int_eq(bluealsa_get_transport_volume(fd, &t, &ch1_muted, &ch1_volume, &ch2_muted, &ch2_volume), 0);
	ck_assert_int_eq(ch1_muted, true);
	ck_assert_int_eq(ch1_volume, 15);
	ck_assert_int_eq(ch2_muted, true);
	ck_assert_int_eq(ch2_volume, 50);

	close(fd);
	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_open_transport) {

	const char *hci = "hci-tc4";
	pid_t pid = spawn_bluealsa_server(hci, 2, false, true, false);

	int fd = -1;
	ck_assert_int_ne(fd = bluealsa_open(hci), -1);

	struct ba_msg_transport t0, t1;
	ck_assert_int_ne(bluealsa_get_transport(fd, &addr0, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK, &t0), -1);
	ck_assert_int_ne(bluealsa_get_transport(fd, &addr1, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK, &t1), -1);

	int pcm_fd0 = -1;
	int pcm_fd1 = -1;
	ck_assert_int_ne(pcm_fd0 = bluealsa_open_transport(fd, &t0), -1);
	ck_assert_int_ne(pcm_fd1 = bluealsa_open_transport(fd, &t1), -1);

	close(fd);
	/* ensure that we can reopen PCM after client disconnection */

	ck_assert_int_ne(fd = bluealsa_open(hci), -1);
	ck_assert_int_ne(bluealsa_get_transport(fd, &addr0, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK, &t0), -1);
	ck_assert_int_ne(bluealsa_get_transport(fd, &addr1, BA_PCM_TYPE_A2DP | BA_PCM_STREAM_PLAYBACK, &t1), -1);
	ck_assert_int_ne(pcm_fd0 = bluealsa_open_transport(fd, &t0), -1);
	ck_assert_int_ne(pcm_fd1 = bluealsa_open_transport(fd, &t1), -1);

	ck_assert_int_ne(bluealsa_control_transport(fd, &t0, BA_COMMAND_PCM_PAUSE), -1);
	ck_assert_int_ne(bluealsa_control_transport(fd, &t0, BA_COMMAND_PCM_RESUME), -1);
	ck_assert_int_ne(bluealsa_control_transport(fd, &t0, BA_COMMAND_PCM_DRAIN), -1);
	ck_assert_int_ne(bluealsa_control_transport(fd, &t0, BA_COMMAND_PCM_DROP), -1);

	ck_assert_int_ne(close(pcm_fd0), -1);
	ck_assert_int_ne(close(pcm_fd1), -1);

	/* XXX: PCM closing is an asynchronous call. It is possible, that the
	 *      server will not process close() action right away. Right now it
	 *      is not possible to open PCM more then once. So, in order to pass
	 *      this test we will have to wait some time before reconnection. */
	sleep(1);

	/* ensure that we can reopen closed PCM */
	ck_assert_int_ne(pcm_fd0 = bluealsa_open_transport(fd, &t0), -1);
	ck_assert_int_ne(close(pcm_fd0), -1);

	close(fd);
	waitpid(pid, NULL, 0);

} END_TEST

int main(int argc, char *argv[]) {
	(void)argc;

	/* test-pcm and server-mock shall be placed in the same directory */
	bin_path = dirname(argv[0]);

	str2ba("12:34:56:78:9A:BC", &addr0);
	str2ba("12:34:56:9A:BC:DE", &addr1);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	/* test_subscribe requires at least 5s */
	tcase_set_timeout(tc, 10);

	tcase_add_test(tc, test_open);
	tcase_add_test(tc, test_subscribe);
	tcase_add_test(tc, test_get_devices);
	tcase_add_test(tc, test_get_transport);
	tcase_add_test(tc, test_open_transport);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
