/*
 * bluealsa - io.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_IO_H_
#define BLUEALSA_IO_H_

void *io_thread_a2dp_sbc_forward(void *arg);
void *io_thread_a2dp_sbc_backward(void *arg);

#endif
