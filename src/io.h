/*
 * BlueALSA - io.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_IO_H_
#define BLUEALSA_IO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "ba-transport.h"

int io_thread_create(struct ba_transport *t);

#endif
