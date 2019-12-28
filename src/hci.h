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

/**
 * List of all Bluetooth member companies:
 * https://www.bluetooth.com/specifications/assigned-numbers/company-identifiers */

#define BT_COMPID_INTEL              0x0002
#define BT_COMPID_QUALCOMM_TECH_INTL 0x000A
#define BT_COMPID_BROADCOM           0x000F
#define BT_COMPID_APPLE              0x004C
#define BT_COMPID_APT                0x004F
#define BT_COMPID_SAMSUNG_ELEC       0x0075
#define BT_COMPID_QUALCOMM_TECH      0x00D7
#define BT_COMPID_SONY               0x012D
#define BT_COMPID_CYPRESS            0x0131
#define BT_COMPID_SAVITECH           0x053A

int hci_get_version(int dev_id, struct hci_version *ver);

int hci_sco_open(int dev_id);
int hci_sco_connect(int sco_fd, const bdaddr_t *ba, uint16_t voice);
unsigned int hci_sco_get_mtu(int sco_fd);

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

const char *batostr_(const bdaddr_t *ba);

#endif
