/*
 * test-pcm.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
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
#include "../src/shared/log.h"


static int set_hw_params(snd_pcm_t *pcm, int channels, int rate,
		unsigned int *buffer_time, unsigned int *period_time) {

	snd_pcm_hw_params_t *params;
	int dir;
	int err;

	snd_pcm_hw_params_alloca(&params);

	if ((err = snd_pcm_hw_params_any(pcm, params)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_channels(pcm, params, channels)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_rate(pcm, params, rate, 0)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, params, buffer_time, &dir)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_period_time_near(pcm, params, period_time, &dir)) != 0)
		return err;
	if ((err = snd_pcm_hw_params(pcm, params)) != 0)
		return err;

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

int test_playback(void) {

	int pcm_channels = 2;
	int pcm_sampling = 44100;
	unsigned int pcm_buffer_time = 500000;
	unsigned int pcm_period_time = 100000;

	snd_pcm_t *pcm = NULL;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_sframes_t delay;

	assert(snd_pcm_open(&pcm, "bluealsa:HCI=hci-xxx,DEV=12:34:56:78:9A:BC,DELAY=0", SND_PCM_STREAM_PLAYBACK, 0) == 0);
	assert(set_hw_params(pcm, pcm_channels, pcm_sampling, &pcm_buffer_time, &pcm_period_time) == 0);
	assert(snd_pcm_get_params(pcm, &buffer_size, &period_size) == 0);
	assert(set_sw_params(pcm, buffer_size, period_size) == 0);
	assert(snd_pcm_prepare(pcm) == 0);

	int16_t *buffer = malloc(period_size * pcm_channels);
	int i, x = 0;

	/* fill-in buffer without starting playback */
	int buffer_period_count = (buffer_size - 10) / period_size + 1;
	for (i = 0; i < buffer_period_count - 1; i++) {
		x = snd_pcm_sine_s16le(buffer, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
		assert(snd_pcm_writei(pcm, buffer, period_size) > 0);
	}

	usleep(100000);

	/* check if playback was not started and if delay is correctly calculated */
	assert(snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED);
	assert(snd_pcm_delay(pcm, &delay) == 0);
	assert(delay == 21042);

	/* start playback - start threshold will be exceeded */
	x = snd_pcm_sine_s16le(buffer, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
	assert(snd_pcm_writei(pcm, buffer, period_size) > 0);
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
		x = snd_pcm_sine_s16le(buffer, period_size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
		assert(snd_pcm_writei(pcm, buffer, period_size) > 0);
	}
	assert(snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING);

	return 0;
}

int test_capture(void) {
	return 0;
}

int main(int argc, char *argv[]) {
	(void)argc;

	char *ba_argv[] = { "test-server", "--source", NULL };
	char ba_path[256];
	pid_t ba_pid;

	/* test-pcm and test-server shall be placed in the same directory */
	sprintf(ba_path, "%s/test-server", dirname(argv[0]));
	assert(posix_spawn(&ba_pid, ba_path, NULL, NULL, ba_argv, NULL) == 0);
	usleep(100000);

	test_run(test_playback);
	test_run(test_capture);

	wait(NULL);
	return EXIT_SUCCESS;
}
