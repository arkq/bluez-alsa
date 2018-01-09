/*
 * test-pcm.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <libgen.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <alsa/asoundlib.h>
#include "inc/sine.inc"
#include "inc/test.inc"
#include "../src/shared/ffb.c"
#include "../src/shared/log.h"


static char *bin_path = NULL;
static pid_t bluealsa_pid = 0;

static pid_t start_bluealsa(bool source, unsigned int timeout) {

	char path[256];
	char arg_timeout[16];
	pid_t pid;

	sprintf(arg_timeout, "--timeout=%d", timeout);

	char *argv[] = {
		"test-server",
		source ? "--source" : "--sink",
		arg_timeout,
		NULL,
	};

	sprintf(path, "%s/test-server", bin_path);
	/* assert(posix_spawn(&pid, path, NULL, NULL, argv, NULL) == 0); */

	/* XXX: workaround for valgrind-3.13.0 */
	if ((pid = fork()) == 0)
		execv(path, argv);

	usleep(100000);
	return pid;
}

static int snd_pcm_open_bluealsa(snd_pcm_t **pcmp, snd_pcm_stream_t stream, int mode) {

	static char buffer[] =
		"pcm.bluealsa {\n"
		"  type bluealsa\n"
		"  interface \"hci-xxx\"\n"
		"  device \"12:34:56:78:9A:BC\"\n"
		"  profile \"a2dp\"\n"
		"  delay 0\n"
		"}\n";

	snd_config_t *conf = NULL;
	snd_input_t *input = NULL;
	int err;

	if ((err = snd_config_top(&conf)) < 0)
		goto fail;
	if ((err = snd_input_buffer_open(&input, buffer, sizeof(buffer) - 1)) != 0)
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

int test_playback_hw_constraints(void) {

	/* hard-coded values used in the test-server */
	const unsigned int server_channels = 2;
	const unsigned int server_rate = 44100;

	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *params;
	int d;

	assert(snd_pcm_open_bluealsa(&pcm, SND_PCM_STREAM_PLAYBACK, 0) == 0);

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_hw_params_any(pcm, params);

	assert(snd_pcm_hw_params_test_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) == 0);
	assert(snd_pcm_hw_params_test_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0);
	assert(snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) == 0);

	snd_pcm_format_t format;
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_format_first(pcm, params, &format) == 0 && format == SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_format_last(pcm, params, &format) == 0 && format == SND_PCM_FORMAT_S16_LE);

	unsigned int channels;
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_channels_first(pcm, params, &channels) == 0 && channels == server_channels);
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_channels_last(pcm, params, &channels) == 0 && channels == server_channels);

	unsigned int rate;
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_rate_first(pcm, params, &rate, &d) == 0 && rate == server_rate && d == 0);
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_rate_last(pcm, params, &rate, &d) == 0 && rate == server_rate && d == 0);

	unsigned int periods;
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_periods_first(pcm, params, &periods, &d) == 0 && periods == 3 && d == 0);
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_periods_last(pcm, params, &periods, &d) == 0 && periods == 1024 && d == 0);

	unsigned int time;
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_buffer_time_first(pcm, params, &time, &d) == 0 && time == 200000 && d == 0);
	snd_pcm_hw_params_any(pcm, params);
	assert(snd_pcm_hw_params_set_buffer_time_last(pcm, params, &time, &d) == 0 && time == 95108934 && d == 1);

	assert(snd_pcm_close(pcm) == 0);
	return 0;
}

int test_playback(void) {

	int pcm_channels = 2;
	int pcm_sampling = 44100;
	unsigned int pcm_buffer_time = 500000;
	unsigned int pcm_period_time = 100000;

	snd_pcm_t *pcm = NULL;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_sframes_t delay;

	assert(snd_pcm_open_bluealsa(&pcm, SND_PCM_STREAM_PLAYBACK, 0) == 0);
	assert(set_hw_params(pcm, pcm_channels, pcm_sampling, &pcm_buffer_time, &pcm_period_time) == 0);
	assert(snd_pcm_get_params(pcm, &buffer_size, &period_size) == 0);
	assert(set_sw_params(pcm, buffer_size, period_size) == 0);
	assert(snd_pcm_prepare(pcm) == 0);

	int16_t *period = malloc(period_size * pcm_channels * sizeof(int16_t));
	int i, x = 0;

	/* fill-in buffer without starting playback */
	int buffer_period_count = (buffer_size - 10) / period_size + 1;
	for (i = 0; i < buffer_period_count - 1; i++) {
		x = snd_pcm_sine_s16le(period, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
		assert(snd_pcm_writei(pcm, period, period_size) > 0);
	}

	usleep(100000);

	/* check if playback was not started and if delay is correctly calculated */
	assert(snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED);
	assert(snd_pcm_delay(pcm, &delay) == 0);
	assert(delay == 18375);

	/* start playback - start threshold will be exceeded */
	x = snd_pcm_sine_s16le(period, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
	assert(snd_pcm_writei(pcm, period, period_size) > 0);
	assert(snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING);

	/* at this point buffer should be still almost full */
	assert(snd_pcm_delay(pcm, &delay) == 0);
	assert(delay > 20000);

	snd_pcm_pause(pcm, 1);
	assert(snd_pcm_state(pcm) == SND_PCM_STATE_PAUSED);

	/* during pause buffer shall not be consumed */
	usleep(100000);
	assert(snd_pcm_delay(pcm, &delay) == 0);
	assert(delay > 10000);

	snd_pcm_pause(pcm, 0);
	assert(snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING);

	/* allow under-run to occur */
	usleep(500000);
	assert(snd_pcm_state(pcm) == SND_PCM_STATE_XRUN);

	/* check successful recovery */
	assert(snd_pcm_prepare(pcm) == 0);
	for (i = 0; i < buffer_period_count * 2; i++) {
		x = snd_pcm_sine_s16le(period, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
		assert(snd_pcm_writei(pcm, period, period_size) > 0);
	}
	assert(snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING);

	assert(snd_pcm_close(pcm) == 0);

	free(period);
	return 0;
}

int test_playback_termination(void) {

	int pcm_channels = 2;
	int pcm_sampling = 44100;
	unsigned int pcm_buffer_time = 500000;
	unsigned int pcm_period_time = 100000;

	snd_pcm_t *pcm = NULL;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;

	assert(snd_pcm_open_bluealsa(&pcm, SND_PCM_STREAM_PLAYBACK, 0) == 0);
	assert(set_hw_params(pcm, pcm_channels, pcm_sampling, &pcm_buffer_time, &pcm_period_time) == 0);
	assert(snd_pcm_get_params(pcm, &buffer_size, &period_size) == 0);
	assert(snd_pcm_prepare(pcm) == 0);

	int16_t *period = malloc(period_size * pcm_channels * sizeof(int16_t));
	snd_pcm_sframes_t frames = 0;
	size_t i = 0;

	/* write samples until server disconnects */
	while (frames >= 0) {
		if (i++ == 10)
			kill(bluealsa_pid, SIGUSR2);
		frames = snd_pcm_writei(pcm, period, period_size);
	}

	/* check if most commonly used calls will report missing device */

	struct pollfd pfds[2];
	unsigned short revents;

	assert(frames == -ENODEV);
	assert(snd_pcm_poll_descriptors_revents(pcm, pfds, 2, &revents) == -ENODEV);
	assert(snd_pcm_writei(pcm, period, period_size) == -ENODEV);
	assert(snd_pcm_avail_update(pcm) == -ENODEV);
	assert(snd_pcm_delay(pcm, &frames) == -ENODEV);
	assert(snd_pcm_prepare(pcm) == -EBADFD);
	assert(snd_pcm_close(pcm) == -EACCES);

	free(period);
	return 0;
}

int test_capture(void) {
	return 0;
}

int main(int argc, char *argv[]) {
	(void)argc;

	/* test-pcm and test-server shall be placed in the same directory */
	bin_path = dirname(argv[0]);

	bluealsa_pid = start_bluealsa(true, 3);
	test_run(test_playback_hw_constraints);
	test_run(test_playback);
	waitpid(bluealsa_pid, NULL, 0);

	bluealsa_pid = start_bluealsa(true, 2);
	test_run(test_playback_termination);
	waitpid(bluealsa_pid, NULL, 0);

	return EXIT_SUCCESS;
}
