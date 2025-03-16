/*
 * BlueALSA - delay-report.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "delay-report.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <alsa/asoundlib.h>
#include <dbus/dbus.h>

#include "alsa-pcm.h"
#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/rt.h"

void delay_report_init(
		struct delay_report *dr,
		struct ba_dbus_ctx *dbus_ctx,
		struct ba_pcm *ba_pcm) {

	memset(dr, 0, sizeof(*dr));
	dr->dbus_ctx = dbus_ctx;
	dr->ba_pcm = ba_pcm;

}

void delay_report_reset(
		struct delay_report *dr) {
	memset(dr->values, 0, sizeof(dr->values));
}

bool delay_report_update(
		struct delay_report *dr,
		struct alsa_pcm *pcm,
		int ba_pcm_fd,
		ffb_t *buffer,
		DBusError *err) {

	unsigned int ba_pcm_buffered = 0;
	/* Get the delay due to BlueALSA PCM FIFO buffering. */
	ioctl(ba_pcm_fd, FIONREAD, &ba_pcm_buffered);
	snd_pcm_sframes_t ba_pcm_frames = ba_pcm_buffered / pcm->frame_size;

	/* Get the delay due to internal buffering. */
	snd_pcm_sframes_t buffer_frames = ffb_blen_out(buffer) / pcm->frame_size;

	const size_t num_values = ARRAYSIZE(dr->values);
	/* Store the delay calculated from all components. */
	dr->values[dr->values_i % num_values] = pcm->delay + ba_pcm_frames + buffer_frames;
	dr->values_i++;

	struct timespec ts_now;
	/* Rate limit delay updates to 1 update per second. */
	struct timespec ts_delay = { .tv_sec = 1 };

	gettimestamp(&ts_now);
	timespecadd(&dr->update_ts, &ts_delay, &ts_delay);

	snd_pcm_sframes_t delay_frames_avg = 0;
	for (size_t i = 0; i < num_values; i++)
		delay_frames_avg += dr->values[i];
	delay_frames_avg /= num_values;

	const int delay = delay_frames_avg * 10000 / dr->ba_pcm->rate;
	if (difftimespec(&ts_now, &ts_delay, &ts_delay) >= 0 ||
			abs(delay - dr->ba_pcm->client_delay) < 100 /* 10ms */)
		return true;

	dr->update_ts = ts_now;
	dr->ba_pcm->client_delay = delay;
	return ba_dbus_pcm_update(dr->dbus_ctx, dr->ba_pcm, BLUEALSA_PCM_CLIENT_DELAY, err);
}
