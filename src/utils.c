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
int a2dp_sbc_default_bitpool(int freq, int mode) {
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

/**
 * Convert Bluetooth address into a human-readable string.
 *
 * This function returns statically allocated buffer. It is not by any means
 * thread safe. This function should be used for debugging purposes only.
 *
 * For debugging purposes, one could use the batostr() function provided by
 * the bluez library. However, this function converts the Bluetooth address
 * to the string with a reversed bytes order...
 *
 * @param ba Pointer to the Bluetooth address structure.
 * @return On success this function returns statically allocated buffer with
 *   human-readable Bluetooth address. On error, it returns NULL. */
const char *batostr_(const bdaddr_t *ba) {
	static char addr[18];
	if (ba2str(ba, addr) > 0)
		return addr;
	return NULL;
}

/**
 * Mute PCM signal stored in the buffer.
 *
 * @param buffer Address to the buffer where the PCM signal is stored.
 * @param size The size of the buffer in bytes.
 * @return The number of audio samples which has been modified. */
int snd_pcm_mute_s16le(void *buffer, size_t size) {
	memset(buffer, 0, size);
	return size / sizeof(int16_t);
}

/**
 * Scale PCM signal stored in the buffer.
 *
 * @param buffer Address to the buffer where the PCM signal is stored.
 * @param size The size of the buffer in bytes.
 * @param scale The scaling factor - 100 is a neutral value.
 * @return The number of audio samples which has been modified. */
int snd_pcm_scale_s16le(void *buffer, size_t size, int scale) {
	const size_t samples = size / sizeof(int16_t);
	size_t i = samples;
	while (i--)
		((int16_t *)buffer)[i] = ((int32_t)((int16_t *)buffer)[i] * scale) / 100;
	return samples;
}
