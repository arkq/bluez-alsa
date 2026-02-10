/*
 * BlueALSA - dumper.h
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 *
 * This library provides functionality to dump Bluetooth audio packets to
 * a simple text file. The format of the dump file is as follows:
 *
 * <PROFILE-TYPE>:<CODEC-ID>[:<CODEC-CONFIGURATION>]
 * <PACKET-SIZE> <PACKET>
 */

#pragma once
#ifndef BLUEALSA_DUMPER_DUMPER_H_
#define BLUEALSA_DUMPER_DUMPER_H_

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "ba-transport.h"
#include "ba-transport-pcm.h"

/**
 * Convert a transport profile to a profile mask. */
unsigned int ba_dumper_profile_to_mask(
		enum ba_transport_profile profile);

/**
 * Convert a profile mask string to a profile mask. */
unsigned int ba_dumper_profile_mask_from_string(
		const char * name);

/**
 * Convert a profile mask to a string. */
const char * ba_dumper_profile_mask_to_string(
		unsigned int mask);

/**
 * Read dump header from a BlueALSA dump file.
 *
 * @param stream Opened file stream.
 * @param profile_mask Profile mask for the transport.
 * @param codec_id BlueALSA codec ID.
 * @param configuration Optional codec configuration (e.g. A2DP caps).
 * @param configuration_size Size of the codec configuration.
 * @return Number of bytes read from the stream, or -1 on error. */
ssize_t ba_dumper_read_header(
		FILE * stream,
		uint32_t * profile_mask,
		uint32_t * codec_id,
		void * configuration,
		size_t * configuration_size);

/**
 * Write dump header to a BlueALSA dump file.
 *
 * @param stream Opened file stream.
 * @param t Transport for which the header should be written.
 * @return Number of bytes written to the stream, or -1 on error. */
ssize_t ba_dumper_write_header(
		FILE * stream,
		const struct ba_transport * t);

/**
 * Read a single BT packet from a BlueALSA dump file.
 *
 * @param stream Opened file stream.
 * @param data Buffer to store the read data.
 * @param size The size of the buffer.
 * @return Number of bytes stored in the buffer, or -1 on error. */
ssize_t ba_dumper_read(
		FILE * stream,
		void * data,
		size_t size);

/**
 * Write a single BT packet to a BlueALSA dump file.
 *
 * @param stream Opened file stream.
 * @param data Buffer containing the data to write.
 * @param size The size of the buffer.
 * @return Number of bytes written to the stream, or -1 on error. */
ssize_t ba_dumper_write(
		FILE * stream,
		const void * data,
		size_t size);

/**
 * BlueALSA transport PCM IO function for dumping incoming BT data. */
void * ba_dumper_io_thread(
		struct ba_transport_pcm * t_pcm);

/**
 * Get a string representation of the transport.
 *
 * The returned string is stored in a static buffer, so it will be overwritten
 * on the next call to this function.
 *
 * @param t The transport to get the name for.
 * @return The transport string. */
const char * ba_transport_to_string(
		const struct ba_transport * t);

/**
 * Get a string representation of the transport PCM.
 *
 * The returned string is stored in a static buffer, so it will be overwritten
 * on the next call to this function.
 *
 * @param t_pcm The transport PCM to get the name for.
 * @return The transport PCM string. */
const char * ba_transport_pcm_to_string(
		const struct ba_transport_pcm * t_pcm);

#endif
