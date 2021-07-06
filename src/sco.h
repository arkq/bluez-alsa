/*
 * BlueALSA - sco.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SCO_H_
#define BLUEALSA_SCO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-adapter.h"
#include "ba-transport.h"

int sco_setup_connection_dispatcher(struct ba_adapter *a);

void *sco_enc_thread(struct ba_transport_thread *th);
void *sco_dec_thread(struct ba_transport_thread *th);

#endif
