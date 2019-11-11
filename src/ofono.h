/*
 * BlueALSA - ofono.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *               2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_OFONO_H_
#define BLUEALSA_OFONO_H_

#include <stdbool.h>

int ofono_register(void);
int ofono_subscribe_signals(void);
bool ofono_detect_service(void);

#endif
