/*
 * BlueALSA - bluealsa-skeleton.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-skeleton.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "bluealsa-iface.h"

G_DEFINE_TYPE(bluealsa_ManagerIfaceSkeleton, bluealsa_manager_iface_skeleton,
		G_TYPE_DBUS_INTERFACE_SKELETON);

static void bluealsa_manager_iface_skeleton_class_init(
		bluealsa_ManagerIfaceSkeletonClass *ifc) {
	GDBusInterfaceSkeletonClass *ifc_ = G_DBUS_INTERFACE_SKELETON_CLASS(ifc);
	ifc_->get_info = g_dbus_interface_skeleton_ex_class_get_info;
	ifc_->get_vtable = g_dbus_interface_skeleton_ex_class_get_vtable;
	ifc_->get_properties = g_dbus_interface_skeleton_ex_class_get_properties;
}

static void bluealsa_manager_iface_skeleton_init(
		bluealsa_ManagerIfaceSkeleton *ifs) {
	(void)ifs;
}

/**
 * Create a skeleton for org.bluealsa.Manager1 interface.
 *
 * @return On success, this function returns newly allocated GIO interface
 *   skeleton object, which shall be freed with g_object_unref(). If error
 *   occurs, NULL is returned. */
bluealsa_ManagerIfaceSkeleton *bluealsa_manager_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func) {
	const GType type = bluealsa_manager_iface_skeleton_get_type();
	return g_dbus_interface_skeleton_ex_new(type,
			(GDBusInterfaceInfo *)&org_bluealsa_manager1_interface,
			vtable, userdata, userdata_free_func);
}

G_DEFINE_TYPE(bluealsa_PCMIfaceSkeleton, bluealsa_pcm_iface_skeleton,
		G_TYPE_DBUS_INTERFACE_SKELETON);

static void bluealsa_pcm_iface_skeleton_class_init(
		bluealsa_PCMIfaceSkeletonClass *ifc) {
	GDBusInterfaceSkeletonClass *ifc_ = G_DBUS_INTERFACE_SKELETON_CLASS(ifc);
	ifc_->get_info = g_dbus_interface_skeleton_ex_class_get_info;
	ifc_->get_vtable = g_dbus_interface_skeleton_ex_class_get_vtable;
	ifc_->get_properties = g_dbus_interface_skeleton_ex_class_get_properties;
}

static void bluealsa_pcm_iface_skeleton_init(
		bluealsa_PCMIfaceSkeleton *ifs) {
	g_dbus_interface_skeleton_set_flags(G_DBUS_INTERFACE_SKELETON(ifs),
			G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

/**
 * Create a skeleton for org.bluealsa.PCM1 interface.
 *
 * @return On success, this function returns newly allocated GIO interface
 *   skeleton object, which shall be freed with g_object_unref(). If error
 *   occurs, NULL is returned. */
bluealsa_PCMIfaceSkeleton *bluealsa_pcm_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func) {
	const GType type = bluealsa_pcm_iface_skeleton_get_type();
	return g_dbus_interface_skeleton_ex_new(type,
			(GDBusInterfaceInfo *)&org_bluealsa_pcm1_interface,
			vtable, userdata, userdata_free_func);
}

G_DEFINE_TYPE(bluealsa_RFCOMMIfaceSkeleton, bluealsa_rfcomm_iface_skeleton,
		G_TYPE_DBUS_INTERFACE_SKELETON);

static void bluealsa_rfcomm_iface_skeleton_class_init(
		bluealsa_RFCOMMIfaceSkeletonClass *ifc) {
	GDBusInterfaceSkeletonClass *ifc_ = G_DBUS_INTERFACE_SKELETON_CLASS(ifc);
	ifc_->get_info = g_dbus_interface_skeleton_ex_class_get_info;
	ifc_->get_vtable = g_dbus_interface_skeleton_ex_class_get_vtable;
	ifc_->get_properties = g_dbus_interface_skeleton_ex_class_get_properties;
}

static void bluealsa_rfcomm_iface_skeleton_init(
		bluealsa_RFCOMMIfaceSkeleton *ifs) {
	(void)ifs;
}

/**
 * Create a skeleton for org.bluealsa.RFCOMM1 interface.
 *
 * @return On success, this function returns newly allocated GIO interface
 *   skeleton object, which shall be freed with g_object_unref(). If error
 *   occurs, NULL is returned. */
bluealsa_RFCOMMIfaceSkeleton *bluealsa_rfcomm_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func) {
	const GType type = bluealsa_rfcomm_iface_skeleton_get_type();
	return g_dbus_interface_skeleton_ex_new(type,
			(GDBusInterfaceInfo *)&org_bluealsa_rfcomm1_interface,
			vtable, userdata, userdata_free_func);
}
