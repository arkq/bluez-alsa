/*
 * BlueALSA - hex.h
 * SPDX-FileCopyrightText: 2021-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
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
