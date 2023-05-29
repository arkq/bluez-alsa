/*
 * BlueALSA - ofono-skeleton.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ofono-skeleton.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ofono-iface.h"

G_DEFINE_TYPE(ofono_HFAudioAgentIfaceSkeleton, ofono_hf_audio_agent_iface_skeleton,
		G_TYPE_DBUS_INTERFACE_SKELETON);

static void ofono_hf_audio_agent_iface_skeleton_class_init(
		ofono_HFAudioAgentIfaceSkeletonClass *ifc) {
	GDBusInterfaceSkeletonClass *ifc_ = G_DBUS_INTERFACE_SKELETON_CLASS(ifc);
	ifc_->get_info = g_dbus_interface_skeleton_ex_class_get_info;
	ifc_->get_vtable = g_dbus_interface_skeleton_ex_class_get_vtable;
	ifc_->get_properties = g_dbus_interface_skeleton_ex_class_get_properties;
}

static void ofono_hf_audio_agent_iface_skeleton_init(
		ofono_HFAudioAgentIfaceSkeleton *ifs) {
	(void)ifs;
}

/**
 * Create a skeleton for org.ofono.HandsfreeAudioAgent interface.
 *
 * @return On success, this function returns newly allocated GIO interface
 *   skeleton object, which shall be freed with g_object_unref(). If error
 *   occurs, NULL is returned. */
ofono_HFAudioAgentIfaceSkeleton *ofono_hf_audio_agent_iface_skeleton_new(
		const GDBusInterfaceSkeletonVTable *vtable, void *userdata,
		GDestroyNotify userdata_free_func) {
	const GType type = ofono_hf_audio_agent_iface_skeleton_get_type();
	return g_dbus_interface_skeleton_ex_new(type,
			(GDBusInterfaceInfo *)&org_ofono_handsfree_audio_agent_interface,
			vtable, userdata, userdata_free_func);
}
