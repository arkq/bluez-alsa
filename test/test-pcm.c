/*
 * test-pcm.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <getopt.h>
#include <libgen.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <check.h>
#include <alsa/asoundlib.h>

#include "inc/server.inc"
#include "inc/sine.inc"
#include "../src/shared/ffb.c"
#include "../src/shared/log.c"

#define buffer_test_frames 1024
#define dumprv(fn) fprintf(stderr, #fn " = %d\n", (int)fn)

static int snd_pcm_open_bluealsa(snd_pcm_t **pcmp, const char *hci, snd_pcm_stream_t stream, int mode) {

	char buffer[256];
	snd_config_t *conf = NULL;
	snd_input_t *input = NULL;
	int err;

	sprintf(buffer,
			"pcm.bluealsa {\n"
			"  type bluealsa\n"
			"  interface \"%s\"\n"
			"  device \"12:34:56:78:9A:BC\"\n"
			"  profile \"a2dp\"\n"
			"  delay 0\n"
			"}\n", hci);

	if ((err = snd_config_top(&conf)) < 0)
		goto fail;
	if ((err = snd_input_buffer_open(&input, buffer, strlen(buffer))) != 0)
		goto fail;
	if ((err = snd_config_load(conf, input)) != 0)
		goto fail;
	err = snd_pcm_open_lconf(pcmp, "bluealsa", stream, mode, conf);

fail:
	if (conf != NULL)
		snd_config_delete(conf);
	if (input != NULL)
		snd_input_close(input);
	return err;
}

static int set_hw_params(snd_pcm_t *pcm, int channels, int rate,
		unsigned int *buffer_time, unsigned int *period_time) {

	snd_pcm_hw_params_t *params;
	int dir;
	int err;

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_hw_params_any(pcm, params);

	if ((err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) != 0) {
		error("snd_pcm_hw_params_set_access: %s", snd_strerror(err));
		return err;
	}
	if ((err = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE)) != 0) {
		error("snd_pcm_hw_params_set_format: %s", snd_strerror(err));
		return err;
	}
	if ((err = snd_pcm_hw_params_set_channels(pcm, params, channels)) != 0) {
		error("snd_pcm_hw_params_set_channels: %s", snd_strerror(err));
		return err;
	}
	if ((err = snd_pcm_hw_params_set_rate(pcm, params, rate, 0)) != 0) {
		error("snd_pcm_hw_params_set_rate: %s", snd_strerror(err));
		return err;
	}
	if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, params, buffer_time, &dir)) != 0) {
		error("snd_pcm_hw_params_set_buffer_time_near: %s", snd_strerror(err));
		return err;
	}
	if ((err = snd_pcm_hw_params_set_period_time_near(pcm, params, period_time, &dir)) != 0) {
		error("snd_pcm_hw_params_set_period_time_near: %s", snd_strerror(err));
		return err;
	}
	if ((err = snd_pcm_hw_params(pcm, params)) != 0) {
		error("snd_pcm_hw_params: %s", snd_strerror(err));
		return err;
	}

	return 0;
}

static int set_sw_params(snd_pcm_t *pcm, snd_pcm_uframes_t buffer_size, snd_pcm_uframes_t period_size) {

	snd_pcm_sw_params_t *params;
	int err;

	snd_pcm_sw_params_alloca(&params);

	if ((err = snd_pcm_sw_params_current(pcm, params)) != 0)
		return err;
	/* start the transfer when the buffer is full (or almost full) */
	snd_pcm_uframes_t threshold = (buffer_size / period_size) * period_size;
	if ((err = snd_pcm_sw_params_set_start_threshold(pcm, params, threshold)) != 0)
		return err;
	/* allow the transfer when at least period_size samples can be processed */
	if ((err = snd_pcm_sw_params_set_avail_min(pcm, params, period_size)) != 0)
		return err;
	if ((err = snd_pcm_sw_params(pcm, params)) != 0)
		return err;

	return 0;
}

START_TEST(test_playback_hw_constraints) {

	const char *hci = "hci-ts1";
	pid_t pid = spawn_bluealsa_server(hci, 1, true, false);

	/* hard-coded values used in the server-mock */
	const unsigned int server_channels = 2;
	const unsigned int server_rate = 44100;

	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *params;
	int d;

	ck_assert_int_eq(snd_pcm_open_bluealsa(&pcm, hci, SND_PCM_STREAM_PLAYBACK, 0), 0);

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_hw_params_any(pcm, params);

	ck_assert_int_eq(snd_pcm_hw_params_test_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED), 0);
	ck_assert_int_eq(snd_pcm_hw_params_test_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED), 0);
	ck_assert_int_eq(snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED), 0);

	snd_pcm_format_t format;
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_format_first(pcm, params, &format), 0);
	ck_assert_int_eq(format, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_format_last(pcm, params, &format), 0);
	ck_assert_int_eq(format, SND_PCM_FORMAT_S16_LE);

	unsigned int channels;
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_channels_first(pcm, params, &channels), 0);
	ck_assert_int_eq(channels, server_channels);
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_channels_last(pcm, params, &channels), 0);
	ck_assert_int_eq(channels, server_channels);

	unsigned int rate;
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_rate_first(pcm, params, &rate, &d), 0);
	ck_assert_int_eq(rate, server_rate);
	ck_assert_int_eq(d, 0);
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_rate_last(pcm, params, &rate, &d), 0);
	ck_assert_int_eq(rate, server_rate);
	ck_assert_int_eq(d, 0);

	unsigned int periods;
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_periods_first(pcm, params, &periods, &d), 0);
	ck_assert_int_eq(periods, 3);
	ck_assert_int_eq(d, 0);
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_periods_last(pcm, params, &periods, &d), 0);
	ck_assert_int_eq(periods, 1024);
	ck_assert_int_eq(d, 0);

	unsigned int time;
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_buffer_time_first(pcm, params, &time, &d), 0);
	ck_assert_int_eq(time, 200000);
	ck_assert_int_eq(d, 0);
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_buffer_time_last(pcm, params, &time, &d), 0);
	ck_assert_int_eq(time, 95108934);
	ck_assert_int_eq(d, 1);

	ck_assert_int_eq(snd_pcm_close(pcm), 0);

	waitpid(pid, NULL, 0);

} END_TEST

START_TEST(test_playback) {

	const char *hci = "hci-ts2";
	pid_t pid = spawn_bluealsa_server(hci, 2, true, false);

	int pcm_channels = 2;
	int pcm_sampling = 44100;
	unsigned int pcm_buffer_time = 500000;
	unsigned int pcm_period_time = 100000;

	snd_pcm_t *pcm = NULL;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_sframes_t delay;

	ck_assert_int_eq(snd_pcm_open_bluealsa(&pcm, hci, SND_PCM_STREAM_PLAYBACK, 0), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_channels, pcm_sampling, &pcm_buffer_time, &pcm_period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	ck_assert_int_eq(set_sw_params(pcm, buffer_size, period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	int16_t *period = malloc(period_size * pcm_channels * sizeof(int16_t));
	int i, x = 0;

	/* fill-in buffer without starting playback */
	int buffer_period_count = (buffer_size - 10) / period_size + 1;
	for (i = 0; i < buffer_period_count - 1; i++) {
		x = snd_pcm_sine_s16le(period, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
		ck_assert_int_gt(snd_pcm_writei(pcm, period, period_size), 0);
	}

	usleep(100000);

	/* check if playback was not started and if delay is correctly calculated */
	ck_assert_int_eq(snd_pcm_state(pcm), SND_PCM_STATE_PREPARED);
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	ck_assert_int_eq(delay, 18375);

	/* start playback - start threshold will be exceeded */
	x = snd_pcm_sine_s16le(period, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
	ck_assert_int_gt(snd_pcm_writei(pcm, period, period_size), 0);
	ck_assert_int_eq(snd_pcm_state(pcm), SND_PCM_STATE_RUNNING);

	/* at this point buffer should be still almost full */
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	ck_assert_int_gt(delay, 20000);

	snd_pcm_pause(pcm, 1);
	ck_assert_int_eq(snd_pcm_state(pcm), SND_PCM_STATE_PAUSED);

	/* during pause buffer shall not be consumed */
	usleep(100000);
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	ck_assert_int_gt(delay, 10000);

	snd_pcm_pause(pcm, 0);
	ck_assert_int_eq(snd_pcm_state(pcm), SND_PCM_STATE_RUNNING);

	/* allow under-run to occur */
	usleep(500000);
	ck_assert_int_eq(snd_pcm_state(pcm), SND_PCM_STATE_XRUN);

	/* check successful recovery */
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);
	for (i = 0; i < buffer_period_count * 2; i++) {
		x = snd_pcm_sine_s16le(period, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
		ck_assert_int_gt(snd_pcm_writei(pcm, period, period_size), 0);
	}
	ck_assert_int_eq(snd_pcm_state(pcm), SND_PCM_STATE_RUNNING);

	ck_assert_int_eq(snd_pcm_close(pcm), 0);

	free(period);
	waitpid(pid, NULL, 0);

} END_TEST

/**
 * Make reference test for playback termination.
 *
 * Values obtained with an external USB sound card:
 * - frames = -19
 * - snd_pcm_poll_descriptors_count(pcm) = 1
 * - snd_pcm_poll_descriptors(pcm, pfds, 4) = 1
 * - snd_pcm_poll_descriptors_revents(pcm, pfds, 4, &revents) = 0
 * - snd_pcm_prepare(pcm) = -19
 * - snd_pcm_reset(pcm) = 0
 * - snd_pcm_start(pcm) = -19
 * - snd_pcm_drop(pcm) = -19
 * - snd_pcm_drain(pcm) = -19
 * - snd_pcm_pause(pcm, 0) = -19
 * - snd_pcm_delay(pcm, &frames) = -19
 * - snd_pcm_resume(pcm) = -38
 * - snd_pcm_avail(pcm) = -19
 * - snd_pcm_avail_update(pcm) = 15081
 * - snd_pcm_writei(pcm, buffer, buffer_test_frames) = -19
 * - snd_pcm_wait(pcm, 10) = -19
 * - snd_pcm_close(pcm) = 0
 */
void test_playback_termination_reference(const char *name) {

	snd_pcm_t *pcm = NULL;
	unsigned int pcm_buffer_time = 500000;
	unsigned int pcm_period_time = 100000;
	struct pollfd pfds[2];
	unsigned short revents;
	int err;

	if ((err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, 0)) != 0) {
		error("snd_pcm_open: %s", snd_strerror(err));
		return;
	}
	if (set_hw_params(pcm, 2, 44100, &pcm_buffer_time, &pcm_period_time) != 0)
		return;
	if ((err = snd_pcm_prepare(pcm)) != 0) {
		error("snd_pcm_prepare: %s", snd_strerror(err));
		return;
	}

	int16_t buffer[buffer_test_frames * 2] = { 0 };
	snd_pcm_sframes_t frames = 0;

	fprintf(stderr, "Unplug PCM device...");
	while (frames >= 0)
		frames = snd_pcm_writei(pcm, buffer, buffer_test_frames);
	fprintf(stderr, "\n");

	dumprv(frames);
	dumprv(snd_pcm_poll_descriptors_count(pcm));
	dumprv(snd_pcm_poll_descriptors(pcm, pfds, 4));
	dumprv(snd_pcm_poll_descriptors_revents(pcm, pfds, 4, &revents));
	dumprv(snd_pcm_prepare(pcm));
	dumprv(snd_pcm_reset(pcm));
	dumprv(snd_pcm_start(pcm));
	dumprv(snd_pcm_drop(pcm));
	dumprv(snd_pcm_drain(pcm));
	dumprv(snd_pcm_pause(pcm, 0));
	dumprv(snd_pcm_delay(pcm, &frames));
	dumprv(snd_pcm_resume(pcm));
	dumprv(snd_pcm_avail(pcm));
	dumprv(snd_pcm_avail_update(pcm));
	dumprv(snd_pcm_writei(pcm, buffer, buffer_test_frames));
	dumprv(snd_pcm_wait(pcm, 10));
	dumprv(snd_pcm_close(pcm));

}

START_TEST(test_playback_termination) {

	const char *hci = "hci-ts3";
	pid_t pid = spawn_bluealsa_server(hci, 2, true, false);

	snd_pcm_t *pcm = NULL;
	unsigned int pcm_buffer_time = 500000;
	unsigned int pcm_period_time = 100000;

	ck_assert_int_eq(snd_pcm_open_bluealsa(&pcm, hci, SND_PCM_STREAM_PLAYBACK, 0), 0);
	ck_assert_int_eq(set_hw_params(pcm, 2, 44100, &pcm_buffer_time, &pcm_period_time), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	int16_t buffer[buffer_test_frames * 2] = { 0 };
	snd_pcm_sframes_t frames = 0;
	size_t i = 0;

	/* write samples until server disconnects */
	while (frames >= 0) {
		if (i++ == 10)
			kill(pid, SIGUSR2);
		frames = snd_pcm_writei(pcm, buffer, buffer_test_frames);
	}

	/* check if most commonly used calls will report missing device */

	struct pollfd pfds[4];
	unsigned short revents;

	ck_assert_int_eq(frames, -ENODEV);
	ck_assert_int_eq(snd_pcm_poll_descriptors_count(pcm), 2);
	ck_assert_int_eq(snd_pcm_poll_descriptors(pcm, pfds, 4), 2);
	ck_assert_int_eq(snd_pcm_poll_descriptors_revents(pcm, pfds, 4, &revents), -ENODEV);
	ck_assert_int_eq(snd_pcm_writei(pcm, buffer, buffer_test_frames), -ENODEV);
	ck_assert_int_eq(snd_pcm_avail_update(pcm), -ENODEV);
	ck_assert_int_eq(snd_pcm_delay(pcm, &frames), -ENODEV);
	ck_assert_int_eq(snd_pcm_prepare(pcm), -EBADFD);
	ck_assert_int_eq(snd_pcm_close(pcm), -EACCES);

	waitpid(pid, NULL, 0);

} END_TEST

int test_capture(void) {
	return 0;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "h";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "pcm", required_argument, NULL, 'd' },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("usage: %s [--pcm=NAME]\n", argv[0]);
			return 0;
		case 'd' /* --pcm */ :
			test_playback_termination_reference(optarg);
			return 0;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return 1;
		}

	/* test-pcm and server-mock shall be placed in the same directory */
	bin_path = dirname(argv[0]);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_playback_hw_constraints);
	tcase_add_test(tc, test_playback);
	tcase_add_test(tc, test_playback_termination);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
