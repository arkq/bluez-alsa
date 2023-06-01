/*
 * BlueALSA - hci.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_HCI_H_
#define BLUEALSA_HCI_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h> /* IWYU pragma: keep */
#include <bluetooth/hci_lib.h>

#include "ba-adapter.h"

int hci_get_version(int dev_id, struct hci_version *ver);

/**
 * SCO close-connect quirk delay (milliseconds).
 *
 * Although not documented, it appears that close(2) on a SCO socket returns
 * before the HCI handshake is complete, and as a result opening a new socket
 * immediately after closing one results in an undefined behavior. To avoid
 * this, the close-connect delay shall be used to enforce a delay between the
 * close(2) and connect(2) calls. */
#define HCI_SCO_CLOSE_CONNECT_QUIRK_DELAY 300

int hci_sco_open(int dev_id);
int hci_sco_connect(int sco_fd, const bdaddr_t *ba, uint16_t voice);

unsigned int hci_sco_get_mtu(int sco_fd, struct ba_adapter *a);

#define BT_BCM_PARAM_ROUTING_PCM       0x0
#define BT_BCM_PARAM_ROUTING_TRANSPORT 0x1
#define BT_BCM_PARAM_ROUTING_CODEC     0x2
#define BT_BCM_PARAM_ROUTING_I2S       0x3
#define BT_BCM_PARAM_PCM_CLOCK_128     0x0
#define BT_BCM_PARAM_PCM_CLOCK_256     0x1
#define BT_BCM_PARAM_PCM_CLOCK_512     0x2
#define BT_BCM_PARAM_PCM_CLOCK_1024    0x3
#define BT_BCM_PARAM_PCM_CLOCK_2048    0x4
#define BT_BCM_PARAM_PCM_FRAME_SHORT   0x0
#define BT_BCM_PARAM_PCM_FRAME_LONG    0x1
#define BT_BCM_PARAM_PCM_SYNC_SLAVE    0x0
#define BT_BCM_PARAM_PCM_SYNC_MASTER   0x1
#define BT_BCM_PARAM_PCM_CLK_SLAVE     0x0
#define BT_BCM_PARAM_PCM_CLK_MASTER    0x1

int hci_bcm_read_sco_pcm_params(int dd, uint8_t *routing, uint8_t *clock,
		uint8_t *frame, uint8_t *sync, uint8_t *clk, int to);
int hci_bcm_write_sco_pcm_params(int dd, uint8_t routing, uint8_t clock,
		uint8_t frame, uint8_t sync, uint8_t clk, int to);

#if DEBUG
const char *batostr_(const bdaddr_t *ba);
#endif

#endif
