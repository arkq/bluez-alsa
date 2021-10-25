/*
 * BlueALSA - bluez-skeleton.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluez-skeleton.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "bluez-iface.h"

G_DEFINE_TYPE(bluez_BatteryProviderIfaceSkeleton, bluez_battery_provider_iface_skeleton,
		G_TYPE_DBUS_INTERFACE_SKELETON);

static void bluez_battery_provider_iface_skeleton_class_init(
		bluez_BatteryProviderIfaceSkeletonClass *ifc) {
	GDBusInterfaceSkeletonClass *ifc_ = G_DBUS_INTERFACE_SKELETON_CLASS(ifc);
	ifc_->get_info = g_dbus_interface_skeleton_ex_class_get_info;
	ifc_->get_vtable = g_dbus_interface_skeleton_ex_class_get_vtable;
	ifc_->get_properties = g_dbus_interface_skeleton_ex_class_get_properties;
}

static void bluez_battery_provider_iface_skeleton_init(
		bluez_BatteryProviderIfaceSkeleton *ifs) {
	(void)ifs;
}

/**
 * Create a skeleton for org.bluez.BatteryProvider1 interface.
 *
 * @return On success, this function returns newly allocated GIO interface
 *   skeleton object, which shall be freed with g_object_unref(). If error
 *   occurs, NULL is returned. */
bluez_BatteryProviderIfaceSkeleton *bluez_battery_provider_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func) {
	const GType type = bluez_battery_provider_iface_skeleton_get_type();
	return g_dbus_interface_skeleton_ex_new(type,
			(GDBusInterfaceInfo *)&bluez_iface_battery_provider,
			vtable, userdata, userdata_free_func);
}
