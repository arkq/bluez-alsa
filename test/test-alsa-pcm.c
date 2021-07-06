/*
 * test-alsa-pcm.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <check.h>
#include <alsa/asoundlib.h>

#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"

#include "inc/preload.inc"
#include "inc/server.inc"
#include "inc/sine.inc"

#define dumprv(fn) fprintf(stderr, #fn " = %d\n", (int)fn)

static const char *pcm_device = NULL;
static unsigned int pcm_channels = 2;
static unsigned int pcm_sampling = 44100;
static snd_pcm_format_t pcm_format = SND_PCM_FORMAT_S16_LE;
/* big enough buffer to keep one period of data */
static int16_t buffer[1024 * 8];

static int snd_pcm_open_bluealsa(snd_pcm_t **pcmp, const char *service, snd_pcm_stream_t stream, int mode) {

	char buffer[256];
	snd_config_t *conf = NULL;
	snd_input_t *input = NULL;
	int err;

	sprintf(buffer,
			"pcm.bluealsa {\n"
			"  type bluealsa\n"
			"  service \"org.bluealsa.%s\"\n"
			"  device \"12:34:56:78:9A:BC\"\n"
			"  profile \"a2dp\"\n"
			"  delay 0\n"
			"}\n", service);

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

static int set_hw_params(snd_pcm_t *pcm, snd_pcm_format_t format, int channels,
		int rate, unsigned int *buffer_time, unsigned int *period_time) {

	snd_pcm_hw_params_t *params;
	int dir;
	int err;

	snd_pcm_hw_params_alloca(&params);
	snd_pcm_hw_params_any(pcm, params);

	if ((err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) != 0) {
		error("snd_pcm_hw_params_set_access: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_format(pcm, params, format)) != 0) {
		error("snd_pcm_hw_params_set_format: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_channels(pcm, params, channels)) != 0) {
		error("snd_pcm_hw_params_set_channels: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_rate(pcm, params, rate, 0)) != 0) {
		error("snd_pcm_hw_params_set_rate: %s", snd_strerror(err));
		goto fail;
	}
	dir = 0;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, params, buffer_time, &dir)) != 0) {
		error("snd_pcm_hw_params_set_buffer_time_near: %s", snd_strerror(err));
		goto fail;
	}
	dir = 0;
	if ((err = snd_pcm_hw_params_set_period_time_near(pcm, params, period_time, &dir)) != 0) {
		error("snd_pcm_hw_params_set_period_time_near: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params(pcm, params)) != 0) {
		error("snd_pcm_hw_params: %s", snd_strerror(err));
		goto fail;
	}

	debug("Selected PCM parameters: buffer time: %u us, period time: %u us",
			*buffer_time, *period_time);
	return 0;

fail:
	if (pcm_device != NULL)
		error("List available HW parameters with: aplay -D %s --dump-hw-params /dev/zero",
				pcm_device);
	return err;
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

static int test_pcm_open(pid_t *pid, snd_pcm_t **pcm, snd_pcm_stream_t stream) {

	if (pcm_device != NULL)
		return snd_pcm_open(pcm, pcm_device, stream, 0);

	const char *service = "test";
	if ((*pid = spawn_bluealsa_server(service, 1, true, false,
					stream == SND_PCM_STREAM_PLAYBACK,
					stream == SND_PCM_STREAM_CAPTURE)) == -1)
		return -1;
	return snd_pcm_open_bluealsa(pcm, service, stream, 0);
}

static int test_pcm_close(pid_t pid, snd_pcm_t *pcm) {
	int rv = snd_pcm_close(pcm);
	if (pid != -1) {
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
	}
	return rv;
}

static int16_t *test_sine_s16le(snd_pcm_uframes_t size) {
	static size_t x = 0;
	assert(ARRAYSIZE(buffer) >= size * pcm_channels);
	x = snd_pcm_sine_s16le(buffer, size * pcm_channels, pcm_channels, x, 441.0 / pcm_sampling);
	return buffer;
}

static snd_pcm_state_t snd_pcm_state_runtime(snd_pcm_t *pcm) {
	snd_pcm_status_t *status;
	snd_pcm_status_alloca(&status);
	int rv;
	if ((rv = snd_pcm_status(pcm, status)) != 0)
		return rv;
	return snd_pcm_status_get_state(status);
}

START_TEST(dump_capture) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	snd_output_t *output;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;

	ck_assert_int_eq(snd_output_stdio_attach(&output, stdout, 0), 0);
	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_CAPTURE), 0);

	ck_assert_int_eq(snd_pcm_dump(pcm, output), 0);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_capture_start) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_sframes_t delay;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;
	size_t i;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_CAPTURE), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	/* check capture PCM initial state - not running */
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_PREPARED);
	ck_assert_int_eq(snd_pcm_avail(pcm), 0);
	/* if PCM is not started there should be no delay */
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	ck_assert_int_eq(delay, 0);

	/* start capture PCM without reading data */
	ck_assert_int_eq(snd_pcm_start(pcm), 0);
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_RUNNING);

	/* wait two and a half period time just to make sure that
	 * at least two periods of frames should be available */
	usleep(5 * period_time / 2);

	/* verify the amount of accumulated data */
	ck_assert_int_ge(snd_pcm_avail(pcm), 2 * period_size);
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	ck_assert_int_ge(delay, 2 * period_size);

	/* read few periods from capture PCM */
	for (i = 0; i < buffer_size / period_size; i++)
		ck_assert_int_eq(snd_pcm_readi(pcm, buffer, period_size), period_size);

	/* after reading there should be no more than one period of data in buffer */
	snd_pcm_sframes_t avail;
	ck_assert_int_le((avail = snd_pcm_avail(pcm)), period_size);
	/* but there may be more data in the FIFO */
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	ck_assert_int_ge(delay, avail);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_capture_pause) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_CAPTURE), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);
	ck_assert_int_eq(snd_pcm_start(pcm), 0);

	/* wait one and a half period time just to make sure that
	 * at least one period of frames should be available */
	usleep(3 * period_time / 2);

	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	ck_assert_int_eq(snd_pcm_hw_params_current(pcm, params), 0);

	if (!snd_pcm_hw_params_can_pause(params))
		warn("PCM capture pause test: %s", "PCM can not pause");
	else {

		/* pause capture  */
		ck_assert_int_eq(snd_pcm_pause(pcm, 1), 0);
		ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_PAUSED);

		/* record PCM parameters just after pausing */
		snd_pcm_sframes_t delay0, delay;
		snd_pcm_uframes_t frames0 = snd_pcm_avail(pcm);
		ck_assert_int_eq(snd_pcm_delay(pcm, &delay0), 0);

		/* wait some time */
		usleep(buffer_time);

		/* during pause PCM parameters shall not be modified */
		ck_assert_int_eq(snd_pcm_avail(pcm), frames0);
		ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
		ck_assert_int_eq(delay, delay0);

		/* resume capture */
		ck_assert_int_eq(snd_pcm_pause(pcm, 0), 0);
		ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_RUNNING);

		/* wait a little bit */
		usleep(period_time);

		/* check resume: more available frames, bigger delay */
		ck_assert_int_gt(snd_pcm_avail(pcm), frames0);
		ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
		ck_assert_int_gt(delay, delay0);

	}

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_capture_overrun) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;
	size_t i;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_CAPTURE), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);
	ck_assert_int_eq(snd_pcm_start(pcm), 0);

	/* check that PCM is running and we can read from it */
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_RUNNING);
	ck_assert_int_eq(snd_pcm_readi(pcm, buffer, period_size), period_size);

	/* allow overrun to occur */
	usleep(buffer_time + period_time);

	/* check whether ALSA reports overrun */
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_XRUN);
	ck_assert_int_eq(snd_pcm_avail(pcm), -EPIPE);

	/* recover from overrun */
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_PREPARED);

	/* start capture PCM */
	ck_assert_int_eq(snd_pcm_start(pcm), 0);
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_RUNNING);

	/* make sure that PCM is indeed readable */
	for (i = 0; i < buffer_size / period_size; i++)
		ck_assert_int_eq(snd_pcm_readi(pcm, buffer, period_size), period_size);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_capture_poll) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_CAPTURE), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);

	struct pollfd pfds[8];
	unsigned short revents;
	int count = snd_pcm_poll_descriptors_count(pcm);
	ck_assert_int_eq(snd_pcm_poll_descriptors(pcm, pfds, ARRAYSIZE(pfds)), count);

	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);
	/* for a capture PCM just after prepare, the poll() call shall block
	 * forever or at least the dispatched event shall be set to 0 */
	ck_assert_int_ne(poll(pfds, count, 250), -1);
	snd_pcm_poll_descriptors_revents(pcm, pfds, count, &revents);
	ck_assert_int_eq(revents, 0);

	/* make sure that further calls to poll() will actually block */
	ck_assert_int_eq(poll(pfds, count, 250), 0);

	ck_assert_int_eq(snd_pcm_start(pcm), 0);
	do { /* started capture PCM shall not block forever */
		ck_assert_int_gt(poll(pfds, count, -1), 0);
		snd_pcm_poll_descriptors_revents(pcm, pfds, count, &revents);
	} while (revents == 0);
	/* we should get read event flag set */
	ck_assert_int_eq(revents & POLLIN, POLLIN);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(dump_playback) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_output_t *output;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;

	ck_assert_int_eq(snd_output_stdio_attach(&output, stdout, 0), 0);
	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);

	ck_assert_int_eq(snd_pcm_dump(pcm, output), 0);

	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);

	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	ck_assert_int_eq(snd_pcm_hw_params_current(pcm, params), 0);

	dumprv(snd_pcm_hw_params_can_disable_period_wakeup(params));
	dumprv(snd_pcm_hw_params_can_mmap_sample_resolution(params));
	dumprv(snd_pcm_hw_params_can_overrange(params));
	dumprv(snd_pcm_hw_params_can_pause(params));
	dumprv(snd_pcm_hw_params_can_resume(params));
	dumprv(snd_pcm_hw_params_can_sync_start(params));

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(ba_test_playback_hw_constraints) {

	if (pcm_device != NULL)
		return;

	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	/* hard-coded values used in the bluealsa-mock */
	const unsigned int server_channels = 2;
	const unsigned int server_rate = 44100;

	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *params;
	pid_t pid = -1;
	int d;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);

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
	ck_assert_int_eq(periods, 2);
	ck_assert_int_eq(d, 0);
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_periods_last(pcm, params, &periods, &d), 0);
	ck_assert_int_eq(periods, 1024);
	ck_assert_int_eq(d, 0);

	unsigned int time;
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_buffer_time_first(pcm, params, &time, &d), 0);
	ck_assert_int_eq(time, 20000);
	ck_assert_int_eq(d, 0);
	snd_pcm_hw_params_any(pcm, params);
	ck_assert_int_eq(snd_pcm_hw_params_set_buffer_time_last(pcm, params, &time, &d), 0);
	ck_assert_int_eq(time, 11888616);
	ck_assert_int_eq(d, 1);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_playback_hw_set_free) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;
	size_t i;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);

	for (i = 0; i < 5; i++) {
		int set_hw_param_ret;
		/* acquire Bluetooth transport */
		if ((set_hw_param_ret = set_hw_params(pcm, pcm_format, pcm_channels,
					pcm_sampling, &buffer_time, &period_time)) == -EBUSY) {
			debug("Retrying snd_pcm_hw_params_set...");
			/* do not treat busy as an error */
			i--;
			continue;
		}
		ck_assert_int_eq(set_hw_param_ret, 0);
		/* release Bluetooth transport */
		ck_assert_int_eq(snd_pcm_hw_free(pcm), 0);
	}

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_playback_start) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_sframes_t delay;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;
	size_t i;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	/* setup PCM to be started by writing the last period of data */
	ck_assert_int_eq(set_sw_params(pcm, buffer_size, period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	/* fill-in buffer without starting playback */
	for (i = 0; i < (buffer_size - 10) / period_size; i++)
		ck_assert_int_eq(snd_pcm_writei(pcm, test_sine_s16le(period_size), period_size), period_size);

	/* wait some time to make sure playback was not started */
	usleep(period_time);

	/* check if playback was not started */
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_PREPARED);
	/* check if buffer fullness is at the next-to-last period */
	ck_assert_int_le(snd_pcm_avail(pcm), buffer_size - (i - 1) * period_size);
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	ck_assert_int_ge(delay, (i - 1) * period_size);

	/* start playback - start threshold will be exceeded */
	ck_assert_int_eq(snd_pcm_writei(pcm, test_sine_s16le(period_size), period_size), period_size);
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_RUNNING);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_playback_drain) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	struct timespec t0, t, diff;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;
	size_t i;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	/* get current timestamp */
	gettimestamp(&t0);

	/* fill-in entire PCM buffer */
	for (i = 0; i <= buffer_size / period_size; i++)
		ck_assert_int_eq(snd_pcm_writei(pcm, test_sine_s16le(period_size), period_size), period_size);

	/* drain PCM buffer and stop playback */
	ck_assert_int_eq(snd_pcm_drain(pcm), 0);
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_SETUP);

	gettimestamp(&t);
	difftimespec(&t0, &t, &diff);
	/* verify whether elapsed time is at least PCM buffer time length */
	ck_assert_uint_gt(diff.tv_sec * 1000000 + diff.tv_nsec / 1000, buffer_time);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_playback_pause) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;
	size_t i;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	ck_assert_int_eq(snd_pcm_hw_params_current(pcm, params), 0);

	if (!snd_pcm_hw_params_can_pause(params))
		warn("PCM playback pause test: %s", "PCM can not pause");
	else {

		/* fill-in buffer and start playback */
		for (i = 0; i <= buffer_size / period_size; i++)
			ck_assert_int_eq(snd_pcm_writei(pcm, test_sine_s16le(period_size), period_size), period_size);

		/* pause playback  */
		ck_assert_int_eq(snd_pcm_pause(pcm, 1), 0);
		ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_PAUSED);

		/* record PCM parameters just after pausing */
		snd_pcm_sframes_t delay0, delay;
		snd_pcm_uframes_t frames = snd_pcm_avail(pcm);
		ck_assert_int_eq(snd_pcm_delay(pcm, &delay0), 0);
		ck_assert_int_gt(delay0, 0);

		/* wait some time */
		usleep(buffer_time);

		/* during pause PCM parameters shall not be modified */
		ck_assert_int_eq(snd_pcm_avail(pcm), frames);
		ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
		ck_assert_int_eq(delay, delay0);

		/* resume playback */
		ck_assert_int_eq(snd_pcm_pause(pcm, 0), 0);
		ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_RUNNING);

		/* wait a little bit */
		usleep(3 * period_time / 2);

		/* check resume: more available frames, lower delay */
		ck_assert_int_gt(snd_pcm_avail(pcm), frames);
		ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
		ck_assert_int_lt(delay, delay0);

	}

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_playback_reset) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_sframes_t frames;
	snd_pcm_sframes_t delay;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;
	size_t i;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

retry:

	/* fill-in buffer and start playback */
	for (i = 0; i <= buffer_size / period_size; i++)
		ck_assert_int_eq(snd_pcm_writei(pcm, test_sine_s16le(period_size), period_size), period_size);
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_RUNNING);

	/* there should be less than one period of free space */
	ck_assert_int_lt(snd_pcm_avail(pcm), period_size);
	/* delay should be no less then buffer_size - period_size */
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	ck_assert_int_gt(delay, buffer_size - period_size);

	/* remove queued data from PCM buffer - reset delay to 0 */
	ck_assert_int_eq(snd_pcm_reset(pcm), 0);

	/* immediately try to write one period of data, however, at this point
	 * we might face PCM in the under-run state; do not treat it as an error */
	if ((frames = snd_pcm_writei(pcm, test_sine_s16le(period_size), period_size)) == -EPIPE) {
		debug("Retrying playback reset test: Device in the under-run state");
		ck_assert_int_eq(snd_pcm_prepare(pcm), 0);
		goto retry;
	}

	ck_assert_int_eq(frames, period_size);

	/* verify that only one frame is in the PCM buffer */
	ck_assert_int_ge(snd_pcm_avail(pcm), buffer_size - period_size);
	ck_assert_int_eq(snd_pcm_delay(pcm, &delay), 0);
	/* from the logical point of view there should be no more than one period
	 * of delay, however, hardware PCM device reports a little bit more than
	 * a period of delay, so this test is not as strict as it should be :) */
	ck_assert_int_le(delay, 3 * period_size / 2);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

START_TEST(test_playback_underrun) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;
	size_t i;

	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_get_params(pcm, &buffer_size, &period_size), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	/* fill-in buffer and start playback */
	for (i = 0; i <= buffer_size / period_size; i++)
		ck_assert_int_eq(snd_pcm_writei(pcm, test_sine_s16le(period_size), period_size), period_size);

	/* after one and a half period time we shall be
	 * able to write at least one period frames */
	usleep(3 * period_time / 2);
	ck_assert_int_gt(snd_pcm_avail(pcm), period_size);

	/* allow under-run to occur */
	usleep(buffer_time);

	/* check whether ALSA reports under-run */
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_XRUN);
	ck_assert_int_eq(snd_pcm_avail(pcm), -EPIPE);

	/* recover from under-run */
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	/* check successful recovery */
	for (i = 0; i <= buffer_size / period_size; i++)
		ck_assert_int_eq(snd_pcm_writei(pcm, test_sine_s16le(period_size), period_size), period_size);
	ck_assert_int_eq(snd_pcm_state_runtime(pcm), SND_PCM_STATE_RUNNING);

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

/**
 * Make reference test for device unplug.
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
 * - snd_pcm_writei(pcm, buffer, frames) = -19
 * - snd_pcm_wait(pcm, 10) = -19
 * - snd_pcm_close(pcm) = 0
 */
START_TEST(reference_playback_device_unplug) {
	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_sframes_t frames = 0;
	snd_pcm_t *pcm = NULL;
	struct pollfd pfds[4];
	unsigned short revents;

	/* this test needs user-defined PCM device */
	ck_assert_ptr_ne(pcm_device, NULL);

	ck_assert_int_eq(test_pcm_open(NULL, &pcm, SND_PCM_STREAM_PLAYBACK), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	fprintf(stderr, "Unplug PCM device...");
	while (frames >= 0)
		frames = snd_pcm_writei(pcm, test_sine_s16le(512), 512);
	fprintf(stderr, "\n");

	dumprv(frames);
	dumprv(snd_pcm_poll_descriptors_count(pcm));
	dumprv(snd_pcm_poll_descriptors(pcm, pfds, ARRAYSIZE(pfds)));
	dumprv(snd_pcm_poll_descriptors_revents(pcm, pfds, ARRAYSIZE(pfds), &revents));
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
	dumprv(snd_pcm_writei(pcm, test_sine_s16le(128), 128));
	dumprv(snd_pcm_wait(pcm, 10));
	dumprv(snd_pcm_close(pcm));

} END_TEST

START_TEST(ba_test_playback_device_unplug) {

	if (pcm_device != NULL)
		return;

	fprintf(stderr, "\nSTART TEST: %s (%s:%d)\n", __func__, __FILE__, __LINE__);

	unsigned int buffer_time = 200000;
	unsigned int period_time = 25000;
	snd_pcm_sframes_t frames = 0;
	snd_pcm_t *pcm = NULL;
	pid_t pid = -1;

	ck_assert_ptr_eq(pcm_device, NULL);
	ck_assert_int_eq(test_pcm_open(&pid, &pcm, SND_PCM_STREAM_PLAYBACK), 0);
	ck_assert_int_eq(set_hw_params(pcm, pcm_format, pcm_channels, pcm_sampling,
				&buffer_time, &period_time), 0);
	ck_assert_int_eq(snd_pcm_prepare(pcm), 0);

	/* write samples until server disconnects */
	while (frames >= 0)
		frames = snd_pcm_writei(pcm, test_sine_s16le(128), 128);

#if 0
	/* check if most commonly used calls will report missing device */

	struct pollfd pfds[4];
	unsigned short revents;

	ck_assert_int_eq(frames, -ENODEV);
	ck_assert_int_eq(snd_pcm_poll_descriptors_count(pcm), 2);
	ck_assert_int_eq(snd_pcm_poll_descriptors(pcm, pfds, ARRAYSIZE(pfds)), 2);
	ck_assert_int_eq(snd_pcm_poll_descriptors_revents(pcm, pfds, ARRAYSIZE(pfds), &revents), -ENODEV);
	ck_assert_int_eq(snd_pcm_prepare(pcm), -ENODEV);
	ck_assert_int_eq(snd_pcm_reset(pcm), 0);
	ck_assert_int_eq(snd_pcm_start(pcm), -EBADFD);
	ck_assert_int_eq(snd_pcm_drop(pcm), 0);
	ck_assert_int_eq(snd_pcm_drain(pcm), -EBADFD);
	ck_assert_int_eq(snd_pcm_pause(pcm, 0), -EBADFD);
	ck_assert_int_eq(snd_pcm_delay(pcm, &frames), -ENODEV);
	ck_assert_int_eq(snd_pcm_resume(pcm), 0);
	ck_assert_int_eq(snd_pcm_avail(pcm), -EPIPE);
	ck_assert_int_eq(snd_pcm_avail_update(pcm), -EPIPE);
	ck_assert_int_eq(snd_pcm_writei(pcm, test_sine_s16le(128), 128), -EPIPE);
	ck_assert_int_eq(snd_pcm_wait(pcm, 10), -EPIPE);
	ck_assert_int_eq(snd_pcm_close(pcm), 0);
#endif

	ck_assert_int_eq(test_pcm_close(pid, pcm), 0);

} END_TEST

int main(int argc, char *argv[]) {

	preload(argc, argv, ".libs/aloader.so");

	int opt;
	const char *opts = "hD:c:f:r:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "pcm", required_argument, NULL, 'D' },
		{ "channels", required_argument, NULL, 'c' },
		{ "format", required_argument, NULL, 'f' },
		{ "rate", required_argument, NULL, 'r' },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("usage: %s [--pcm=NAME] [playback|capture|unplug]\n", argv[0]);
			return 0;
		case 'D' /* --pcm=NAME */ :
			pcm_device = optarg;
			break;
		case 'c' /* --channels=NUM */ :
			pcm_channels = atoi(optarg);
			break;
		case 'f' /* --format=FMT */ :
			if ((pcm_format = snd_pcm_format_value(optarg)) == -1)
				pcm_format = SND_PCM_FORMAT_U8;
			break;
		case 'r' /* --rate=NUM */ :
			pcm_sampling = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return 1;
		}

	/* test-alsa-pcm and bluealsa-mock shall be placed in the same directory */
	bluealsa_mock_path = dirname(strdup(argv[0]));

	Suite *s = suite_create(__FILE__);
	SRunner *sr = srunner_create(s);

	TCase *tc_capture = tcase_create("capture");
	tcase_add_test(tc_capture, dump_capture);
	tcase_add_test(tc_capture, test_capture_start);
	tcase_add_test(tc_capture, test_capture_pause);
	tcase_add_test(tc_capture, test_capture_overrun);
	tcase_add_test(tc_capture, test_capture_poll);

	TCase *tc_playback = tcase_create("playback");
	tcase_add_test(tc_playback, dump_playback);
	tcase_add_test(tc_playback, ba_test_playback_hw_constraints);
	tcase_add_test(tc_playback, test_playback_hw_set_free);
	tcase_add_test(tc_playback, test_playback_start);
	tcase_add_test(tc_playback, test_playback_drain);
	tcase_add_test(tc_playback, test_playback_pause);
	tcase_add_test(tc_playback, test_playback_reset);
	tcase_add_test(tc_playback, test_playback_underrun);
	tcase_add_test(tc_playback, ba_test_playback_device_unplug);

	TCase *tc_unplug = tcase_create("unplug");
	tcase_add_test(tc_unplug, reference_playback_device_unplug);

	if (argc == optind) {
		suite_add_tcase(s, tc_capture);
		suite_add_tcase(s, tc_playback);
	}
	else {
		for (; optind < argc; optind++) {
			if (strcmp(argv[optind], "capture") == 0)
				suite_add_tcase(s, tc_capture);
			else if (strcmp(argv[optind], "playback") == 0)
				suite_add_tcase(s, tc_playback);
			else if (strcmp(argv[optind], "unplug") == 0)
				suite_add_tcase(s, tc_unplug);
		}
	}

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
