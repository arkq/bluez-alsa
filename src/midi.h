/*
 * BlueALSA - midi.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_MIDI_H_
#define BLUEALSA_MIDI_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-transport.h"

int midi_transport_alsa_seq_create(struct ba_transport *t);
int midi_transport_alsa_seq_delete(struct ba_transport *t);

int midi_transport_start_watch_alsa_seq(struct ba_transport *t);
int midi_transport_start_watch_ble_midi(struct ba_transport *t);

int midi_transport_start(struct ba_transport *t);
int midi_transport_stop(struct ba_transport *t);

#endif
