/*
 * BlueALSA - test-dbus-iface.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_TEST_TESTDBUSIFACE_H_
#define BLUEALSA_TEST_TESTDBUSIFACE_H_

#include <gio/gio.h>
#include <glib.h>

#include "dbus.h"

typedef struct {
	GDBusInterfaceSkeletonEx parent;
} OrgExampleFooSkeleton;

OrgExampleFooSkeleton *org_example_foo_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func);

#endif
