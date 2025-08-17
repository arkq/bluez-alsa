/*
 * test-alsa-midi.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <check.h>
#include <alsa/asoundlib.h>

#include "shared/log.h"

#include "inc/check.inc"
#include "inc/mock.inc"
#include "inc/spawn.inc"

static int test_seq_open(struct spawn_process *sp_ba_mock, snd_seq_t **seq,
		int streams, int mode) {
	if (spawn_bluealsa_mock(sp_ba_mock, NULL, true,
				"--timeout=5000",
				"--profile=midi",
				NULL) == -1)
		return -1;
	return snd_seq_open(seq, "default", streams, mode);
}

static int test_seq_create_port(snd_seq_t *seq) {
	return snd_seq_create_simple_port(seq, NULL,
			SND_SEQ_PORT_CAP_DUPLEX | SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
}

static int test_seq_close(struct spawn_process *sp_ba_mock, snd_seq_t *seq) {
	int rv = 0;
	if (seq != NULL)
		rv = snd_seq_close(seq);
	if (sp_ba_mock != NULL) {
		spawn_terminate(sp_ba_mock, 0);
		spawn_close(sp_ba_mock, NULL);
	}
	return rv;
}

CK_START_TEST(test_port) {

	struct spawn_process sp_ba_mock;
	snd_seq_t *seq = NULL;

	ck_assert_int_eq(test_seq_open(&sp_ba_mock, &seq, SND_SEQ_OPEN_DUPLEX, 0), 0);

	snd_seq_client_info_t *cinfo;
	snd_seq_client_info_alloca(&cinfo);

	snd_seq_port_info_t *pinfo;
	snd_seq_port_info_alloca(&pinfo);

	bool ba_client_found = false;
	bool ba_port_found = false;

	snd_seq_client_info_set_client(cinfo, -1);
	while (snd_seq_query_next_client(seq, cinfo) == 0) {

		if (strcmp(snd_seq_client_info_get_name(cinfo), "BlueALSA") != 0)
			continue;

		ba_client_found = true;

		snd_seq_port_info_set_client(pinfo, snd_seq_client_info_get_client(cinfo));
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(seq, pinfo) == 0) {
			debug("%d:%d - %s", snd_seq_port_info_get_client(pinfo),
					snd_seq_port_info_get_port(pinfo), snd_seq_port_info_get_name(pinfo));
			ba_port_found = true;
		}

	}

	ck_assert_int_eq(ba_client_found, true);
	ck_assert_int_eq(ba_port_found, true);

	ck_assert_int_eq(test_seq_close(&sp_ba_mock, seq), 0);

} CK_END_TEST

CK_START_TEST(test_sequencer) {

	/* delay in second/10 + raw MIDI data */
	static const uint8_t midi[] = {
		0, 0xb1, 0x07, 0x7f,
		0, 0xc1, 0x49,
		0, 0xc2, 0x01,
		1, 0x90, 0x40, 0x46,
		0, 0x90, 0x41, 0x46,
		0, 0x91, 0x50, 0x7f,
		5, 0x80, 0x40, 0x0,
		0, 0x80, 0x41, 0x0,
		15, 0x81, 0x50, 0x0,
	};

	struct spawn_process sp_ba_mock;
	snd_seq_t *seq = NULL;
	int port;

	ck_assert_int_eq(test_seq_open(&sp_ba_mock, &seq, SND_SEQ_OPEN_DUPLEX, 0), 0);
	ck_assert_int_ge(port = test_seq_create_port(seq), 0);

	snd_seq_addr_t ba_seq_addr;
	ck_assert_int_eq(snd_seq_parse_address(seq, &ba_seq_addr, "BlueALSA"), 0);
	ck_assert_int_eq(snd_seq_connect_from(seq, port, ba_seq_addr.client, ba_seq_addr.port), 0);
	ck_assert_int_eq(snd_seq_connect_to(seq, port, ba_seq_addr.client, ba_seq_addr.port), 0);

	snd_midi_event_t *parser;
	ck_assert_int_eq(snd_midi_event_new(1024, &parser), 0);
	snd_midi_event_no_status(parser, 1);

	long encoded = 0;
	for (size_t i = 0; i < sizeof(midi); i += encoded) {
		usleep(midi[i++] * 100000);

		snd_seq_event_t ev = { 0 };
		snd_seq_ev_set_direct(&ev);
		snd_seq_ev_set_subs(&ev);

		encoded = snd_midi_event_encode(parser, &midi[i], sizeof(midi) - i, &ev);
		ck_assert_int_gt(encoded, 0);

		ck_assert_int_gt(snd_seq_event_output_direct(seq, &ev), 0);

		snd_seq_event_t *ev_o;
		uint8_t buf[16];

		ck_assert_int_gt(snd_seq_event_input(seq, &ev_o), 0);
		ck_assert_int_eq(snd_midi_event_decode(parser, buf, sizeof(buf), ev_o), encoded);
		ck_assert_mem_eq(&midi[i], buf, encoded);

	}

	snd_midi_event_free(parser);
	snd_seq_delete_simple_port(seq, port);
	ck_assert_int_eq(test_seq_close(&sp_ba_mock, seq), 0);

} CK_END_TEST

int main(int argc, char *argv[]) {
	(void)argc;

	/* Check whether current host supports ALSA sequencer. If not, then
	 * there is no point in running this test, because it will fail. */
	if (access("/dev/snd/seq", F_OK | R_OK | W_OK) != 0) {
		warn("ALSA sequencer not available, skipping test!");
		return 77 /* magic number for skipping tests */;
	}

	char *argv_0 = strdup(argv[0]);
	snprintf(bluealsad_mock_path, sizeof(bluealsad_mock_path),
			"%s/mock/bluealsad-mock", dirname(argv_0));

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	/* snd_seq_event_input() blocks until the event timestamp is reached,
	 * according to the system clock. The ble_midi_decode() function limits
	 * the time drift from system clock to 500 ms. So in the worst case,
	 * with 9 events to process, a total of 4500 ms will pass just blocked
	 * in calls to snd_seq_event_input(). Added to the total of 2100 ms in
	 * usleep() calls, this gives a worst-case test time of 6600 ms. */
	tcase_set_timeout(tc, 8);

	tcase_add_test(tc, test_port);
	tcase_add_test(tc, test_sequencer);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	free(argv_0);

	return nf == 0 ? 0 : 1;
}
