=====================
org.bluealsa.Manager1
=====================

---------------------------------
Bluetooth Audio Manager D-Bus API
---------------------------------

:Date: August 2024
:Manual section: 7
:Manual group: D-Bus Interface
:Version: $VERSION$

SYNOPSIS
========

:Service:         org.bluealsa[.unique ID]
:Interface:       org.bluealsa.Manager1
:Object path:     [variable prefix]/

DESCRIPTION
===========

This page describes the D-Bus Manager interface of the **bluealsad(8)**
service. The Manager interface exposes some of the run-time properties of the
service daemon.

Properties
----------

string Version [readonly]
    Version of BlueALSA service.

array{string} Adapters [readonly]
    Used HCI adapters. The device names ("hci0", etc.) of Bluetooth adapters
    that the BlueALSA service is using.

array{string} Profiles [readonly]
    Used (enabled) Bluetooth profiles.

array{string} Codecs [readonly]
    Used (enabled) Bluetooth audio codecs. The Bluetooth audio codec names are
    in the format: "<profile-name>:<codec-name>"


COPYRIGHT
=========

Copyright (c) 2016-2024 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``bluealsactl(1)``, ``bluealsad(8)``

Project web site
  https://github.com/arkq/bluez-alsa
