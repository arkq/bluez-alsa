/*
 * BlueALSA - ctl-client.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_CTLCLIENT_H_
#define BLUEALSA_SHARED_CTLCLIENT_H_

#include <stdbool.h>
#include "shared/ctl-proto.h"

int bluealsa_open(const char *interface);

int bluealsa_event_subscribe(int fd, uint8_t mask);
int bluealsa_event_match(const struct ba_msg_transport *transport,
		const struct ba_msg_event *event);

ssize_t bluealsa_get_devices(int fd, struct ba_msg_device **devices);
ssize_t bluealsa_get_transports(int fd, struct ba_msg_transport **transports);

int bluealsa_get_transport(int fd, const bdaddr_t *addr, uint8_t type,
		struct ba_msg_transport *transport);

int bluealsa_get_transport_delay(int fd, const struct ba_msg_transport *transport,
		unsigned int *delay);
int bluealsa_set_transport_delay(int fd, const struct ba_msg_transport *transport,
		unsigned int delay);

int bluealsa_get_transport_volume(int fd, const struct ba_msg_transport *transport,
		bool *ch1_muted, int *ch1_volume, bool *ch2_muted, int *ch2_volume);
int bluealsa_set_transport_volume(int fd, const struct ba_msg_transport *transport,
		bool ch1_muted, int ch1_volume, bool ch2_muted, int ch2_volume);

int bluealsa_open_transport(int fd, const struct ba_msg_transport *transport);
int bluealsa_control_transport(int fd, const struct ba_msg_transport *transport, enum ba_command cmd);

int bluealsa_send_rfcomm_command(int fd, const bdaddr_t *addr, const char *command);

#endif
