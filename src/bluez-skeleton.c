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

#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "shared/log.h"

G_DEFINE_TYPE(bluez_BatteryProviderIfaceSkeleton, bluez_battery_provider_iface_skeleton,
		G_TYPE_DBUS_INTERFACE_SKELETON);

static GVariant *bluez_battery_provider_iface_skeleton_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *interface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)error;

	bluez_BatteryProviderIfaceSkeleton *ifs = (bluez_BatteryProviderIfaceSkeleton *)userdata;
	const struct ba_device *d = ifs->device;

	if (strcmp(property, "Device") == 0)
		return ba_variant_new_device_path(d);
	if (strcmp(property, "Percentage") == 0)
		return ba_variant_new_device_battery(d);
	if (strcmp(property, "Source") == 0)
		return g_variant_new_string("BlueALSA");

	g_assert_not_reached();
	return NULL;
}

static const GDBusInterfaceVTable bluez_battery_provider_iface_skeleton_vtable = {
	.get_property = bluez_battery_provider_iface_skeleton_get_property,
};

static GDBusInterfaceInfo *bluez_battery_provider_iface_skeleton_get_info(
		GDBusInterfaceSkeleton *iface_skeleton) {
	(void)iface_skeleton;
	return (GDBusInterfaceInfo *)&bluez_iface_battery_provider;
}

static GDBusInterfaceVTable *bluez_battery_provider_iface_skeleton_get_vtable(
		GDBusInterfaceSkeleton *iface_skeleton) {
	(void)iface_skeleton;
	return (GDBusInterfaceVTable *)&bluez_battery_provider_iface_skeleton_vtable;
}

static GVariant *bluez_battery_provider_iface_skeleton_get_properties(
		GDBusInterfaceSkeleton *iface_skeleton) {

	bluez_BatteryProviderIfaceSkeleton *ifs = (bluez_BatteryProviderIfaceSkeleton *)iface_skeleton;
	const struct ba_device *d = ifs->device;

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	g_variant_builder_add(&props, "{sv}", "Device", ba_variant_new_device_path(d));
	g_variant_builder_add(&props, "{sv}", "Percentage", ba_variant_new_device_battery(d));
	g_variant_builder_add(&props, "{sv}", "Source", g_variant_new_string("BlueALSA"));

	return g_variant_builder_end(&props);
}

static void bluez_battery_provider_iface_skeleton_class_init(
		bluez_BatteryProviderIfaceSkeletonClass *ifc) {
	GDBusInterfaceSkeletonClass *ifc_ = G_DBUS_INTERFACE_SKELETON_CLASS(ifc);
	ifc_->get_info = bluez_battery_provider_iface_skeleton_get_info;
	ifc_->get_vtable = bluez_battery_provider_iface_skeleton_get_vtable;
	ifc_->get_properties = bluez_battery_provider_iface_skeleton_get_properties;
}

static void bluez_battery_provider_iface_skeleton_init(
		bluez_BatteryProviderIfaceSkeleton *ifs) {
	(void)ifs;
}

static void device_unref_weak_notify(void *userdata, GObject *where_the_object_was) {
	(void)where_the_object_was;
	ba_device_unref((struct ba_device *)userdata);
}

/**
 * Create a skeleton for org.bluez.BatteryProvider1 interface.
 *
 * On success, this function takes a reference on device for the lifetime
 * period of created interface skeleton object.
 *
 * @param device Pointer to the device structure.
 * @return On success, this function returns newly allocated GIO interface
 *   skeleton object, which shall be freed with g_object_unref(). If error
 *   occurs, NULL is returned. */
bluez_BatteryProviderIfaceSkeleton *bluez_battery_provider_iface_skeleton_new(
		struct ba_device *device) {

	bluez_BatteryProviderIfaceSkeleton *ifs;
	if ((ifs = g_object_new(bluez_battery_provider_iface_skeleton_get_type(), NULL)) == NULL)
		return NULL;

	g_object_weak_ref(G_OBJECT(ifs), device_unref_weak_notify, device);
	ifs->device = ba_device_ref(device);

	return ifs;
}
