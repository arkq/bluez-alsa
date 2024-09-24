/*
 * check.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdio.h>

#include <check.h>

#include "shared/defs.h"

/**
 * Wrapper for START_TEST() macro with additional print with test name. */
#define CK_START_TEST(name) START_TEST(name) { \
	fprintf(stderr, "\nTEST: " __FILE__ ":" STRINGIZE(__LINE__) ": " STRINGIZE(name) "\n");

/**
 * Wrapper for END_TEST macro. */
#define CK_END_TEST } END_TEST
