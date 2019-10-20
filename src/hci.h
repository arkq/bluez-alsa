/*
 * BlueALSA - hci.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_HCI_H_
#define BLUEALSA_HCI_H_

#include <stdbool.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#define BT_COMPID_INTEL    2
#define BT_COMPID_BROADCOM 15

int hci_get_version(int dev_id, struct hci_version *ver);
int hci_open_sco(int dev_id, const bdaddr_t *ba, bool transparent);

#define BT_BCM_PARAM_ROUTING_PCM       0x0
#define BT_BCM_PARAM_ROUTING_TRANSPORT 0x1
#define BT_BCM_PARAM_ROUTING_CODEC     0x2
#define BT_BCM_PARAM_ROUTING_I2S       0x3
#define BT_BCM_PARAM_PCM_RATE_128      0x0
#define BT_BCM_PARAM_PCM_RATE_256      0x1
#define BT_BCM_PARAM_PCM_RATE_512      0x2
#define BT_BCM_PARAM_PCM_RATE_1024     0x3
#define BT_BCM_PARAM_PCM_RATE_2048     0x4
#define BT_BCM_PARAM_PCM_FRAME_SHORT   0x0
#define BT_BCM_PARAM_PCM_FRAME_LONG    0x1
#define BT_BCM_PARAM_PCM_SYNC_SLAVE    0x0
#define BT_BCM_PARAM_PCM_SYNC_MASTER   0x1
#define BT_BCM_PARAM_PCM_CLK_SLAVE     0x0
#define BT_BCM_PARAM_PCM_CLK_MASTER    0x1

int hci_bcm_read_sco_pcm_params(int dd, uint8_t *routing, uint8_t *rate,
		uint8_t *frame, uint8_t *sync, uint8_t *clock, int to);
int hci_bcm_write_sco_pcm_params(int dd, uint8_t routing, uint8_t rate,
		uint8_t frame, uint8_t sync, uint8_t clock, int to);

const char *batostr_(const bdaddr_t *ba);

#endif
