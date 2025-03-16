/*
 * BlueALSA - delay-report.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_DELAYREPORT_H_
#define BLUEALSA_APLAY_DELAYREPORT_H_

#include <stdbool.h>
#include <time.h>

#include <alsa/asoundlib.h>
#include <dbus/dbus.h>

#include "alsa-pcm.h"
#include "shared/ffb.h"

struct delay_report {

	struct ba_dbus_ctx *dbus_ctx;

	struct ba_pcm *ba_pcm;

	/* The time-stamp for delay update rate limiting. */
	struct timespec update_ts;

	/* Window buffer for calculating delay moving average. */
	snd_pcm_sframes_t values[64];
	size_t values_i;

};

void delay_report_init(
		struct delay_report *dr,
		struct ba_dbus_ctx *dbus_ctx,
		struct ba_pcm *ba_pcm);

void delay_report_reset(
		struct delay_report *dr);

bool delay_report_update(
		struct delay_report *dr,
		struct alsa_pcm *pcm,
		int ba_pcm_fd,
		ffb_t *buffer,
		DBusError *err);

#endif
