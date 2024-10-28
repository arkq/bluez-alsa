/*
 * BlueALSA - hex.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_HEX_H_
#define BLUEALSA_SHARED_HEX_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <sys/types.h>

ssize_t bin2hex(const void * restrict bin, char * restrict hex, size_t n);
ssize_t hex2bin(const char * restrict hex, void * restrict bin, size_t n);

#endif
