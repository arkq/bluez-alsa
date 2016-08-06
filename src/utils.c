/*
 * bluealsa - utils.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "utils.h"

#include <stdlib.h>
#include <string.h>

#include <bluetooth/hci_lib.h>

#include "a2dp-codecs.h"
#include "log.h"
#include "transport.h"


/**
 * Calculate the optimum bitpool for given parameters.
 *
 * @param freq Sampling frequency.
 * @param model Channel mode.
 * @return Coded SBC bitpool. */
int a2dp_default_bitpool(int freq, int mode) {
	switch (freq) {
	case SBC_SAMPLING_FREQ_16000:
	case SBC_SAMPLING_FREQ_32000:
		return 53;
	case SBC_SAMPLING_FREQ_44100:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 31;
		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 53;
		default:
			warn("Invalid channel mode: %u", mode);
			return 53;
		}
	case SBC_SAMPLING_FREQ_48000:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 29;
		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 51;
		default:
			warn("Invalid channel mode: %u", mode);
			return 51;
		}
	default:
		warn("Invalid sampling freq: %u", freq);
		return 53;
	}
}

/**
 * Get the list of all available HCI controllers.
 *
 * @param di The address to the device info structure pointer, where the list
 *	 of all available devices will be stored. Allocated memory should be freed
 *	 with the free().
 * @param num The address, where the number of initialized device structures
 *	 will be stored.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *	 errno is set to indicate the error. */
int hci_devlist(struct hci_dev_info **di, int *num) {

	int i;

	if ((*di = malloc(HCI_MAX_DEV * sizeof(**di))) == NULL)
		return -1;

	for (i = *num = 0; i < HCI_MAX_DEV; i++)
		if (hci_devinfo(i, &(*di)[*num]) == 0)
			(*num)++;

	return 0;
}

/**
 * Convert D-Bus type into a machine-readable string.
 *
 * @param typecode D-Bus type code.
 * @return Machine-readable string. */
const char *dbus_type_to_string(int typecode) {
	switch (typecode) {
	case DBUS_TYPE_INVALID:
		return "invalid";
	case DBUS_TYPE_BOOLEAN:
		return "boolean";
	case DBUS_TYPE_BYTE:
		return "byte";
	case DBUS_TYPE_INT16:
		return "int16";
	case DBUS_TYPE_UINT16:
		return "uint16";
	case DBUS_TYPE_INT32:
		return "int32";
	case DBUS_TYPE_UINT32:
		return "uint32";
	case DBUS_TYPE_INT64:
		return "int64";
	case DBUS_TYPE_UINT64:
		return "uint64";
	case DBUS_TYPE_DOUBLE:
		return "double";
	case DBUS_TYPE_STRING:
		return "string";
	case DBUS_TYPE_OBJECT_PATH:
		return "object_path";
	case DBUS_TYPE_SIGNATURE:
		return "signature";
	case DBUS_TYPE_STRUCT:
		return "struct";
	case DBUS_TYPE_DICT_ENTRY:
		return "dict_entry";
	case DBUS_TYPE_ARRAY:
		return "array";
	case DBUS_TYPE_VARIANT:
		return "variant";
	case DBUS_TYPE_UNIX_FD:
		return "unix_fd";
	default:
		warn("Unrecognized argument type: %u", typecode);
		return "unknown";
	}
}

/**
 * Convert BlueZ D-Bus device path into a bdaddr_t structure.
 *
 * @param path BlueZ D-Bus device path.
 * @param addr Address where the parsed address will be stored.
 * @return On success this function returns 0. Otherwise, -1 is returned. */
int dbus_devpath_to_bdaddr(const char *path, bdaddr_t *addr) {

	char *tmp, *p;
	int ret;

	if ((path = strrchr(path, '/')) == NULL)
		return -1;
	if ((path = strstr(path, "dev_")) == NULL)
		return -1;
	if ((tmp = strdup(path + 4)) == NULL)
		return -1;

	for (p = tmp; *p != '\0'; p++)
		if (*p == '_')
			*p = ':';

	ret = str2ba(tmp, addr);

	free(tmp);
	return ret;
}

/**
 * Convert Bluetooth profile into a human-readable string.
 *
 * @param profile Bluetooth profile.
 * @param codec Bluetooth profile audio codec.
 * @return Human-readable string. */
const char *bluetooth_profile_to_string(uint8_t profile, uint8_t codec) {
	switch (profile) {
	case TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (codec) {
		case A2DP_CODEC_SBC:
			return "A2DP-SBC Source";
		case A2DP_CODEC_MPEG12:
			return "A2DP-MPEG12 Source";
		case A2DP_CODEC_MPEG24:
			return "A2DP-MPEG24 Source";
		}
		return "A2DP Source";
	case TRANSPORT_PROFILE_A2DP_SINK:
		switch (codec) {
		case A2DP_CODEC_SBC:
			return "A2DP-SBC Sink";
		case A2DP_CODEC_MPEG12:
			return "A2DP-MPEG12 Sink";
		case A2DP_CODEC_MPEG24:
			return "A2DP-MPEG24 Sink";
		}
		return "A2DP Sink";
	case TRANSPORT_PROFILE_HFP:
		return "HFP";
	case TRANSPORT_PROFILE_HSP:
		return "HSP";
	default:
		return "N/A";
	}
}

dbus_bool_t dbus_message_iter_append_dict_variant(DBusMessageIter *iter,
		char *key, int type, const void *value) {

	const char typestr[] = { type, '\0' };
	DBusMessageIter dict, variant;

	dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, typestr, &variant);
	dbus_message_iter_append_basic(&variant, type, &value);
	dbus_message_iter_close_container(&dict, &variant);
	dbus_message_iter_close_container(iter, &dict);

	return TRUE;
}

dbus_bool_t dbus_message_iter_append_dict_array(DBusMessageIter *iter,
		char *key, int type, const void *value, int elements) {

	const char typestr[] = { 'a', type, '\0' };
	DBusMessageIter dict, variant, array;

	dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, typestr, &variant);
	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, &typestr[1], &array);
	dbus_message_iter_append_fixed_array(&array, type, &value, elements);
	dbus_message_iter_close_container(&variant, &array);
	dbus_message_iter_close_container(&dict, &variant);
	dbus_message_iter_close_container(iter, &dict);

	return TRUE;
}
