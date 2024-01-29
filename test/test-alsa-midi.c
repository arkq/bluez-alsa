/*
 * test-alsa-midi.c
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
#include <stdbool.h>
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
				"--timeout=1000",
				"--profile=midi",
				NULL) == -1)
		return -1;
	return snd_seq_open(seq, "default", streams, mode);
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

int main(int argc, char *argv[]) {
	(void)argc;

	/* Check whether current host supports ALSA sequencer. If not, then
	 * there is no point in running this test, because it will fail. */
	if (access("/dev/snd/seq", F_OK) != 0) {
		warn("ALSA sequencer not available, skipping test!");
		return 77 /* magic number for skipping tests */;
	}

	char *argv_0 = strdup(argv[0]);
	snprintf(bluealsa_mock_path, sizeof(bluealsa_mock_path),
			"%s/mock/bluealsa-mock", dirname(argv_0));

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_port);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	free(argv_0);

	return nf == 0 ? 0 : 1;
}
