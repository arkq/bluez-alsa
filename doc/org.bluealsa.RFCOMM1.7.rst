====================
org.bluealsa.RFCOMM1
====================

---------------------------------
Bluetooth Audio RFCOMM D-Bus API
---------------------------------

:Date: August 2024
:Manual section: 7
:Manual group: D-Bus Interface
:Version: $VERSION$

SYNOPSIS
========

:Service:       org.bluealsa[.unique ID]
:Interface:     org.bluealsa.RFCOMM1
:Object path:   [variable prefix]/{hci0,hci1,...}/dev_XX_XX_XX_XX_XX_XX/rfcomm

DESCRIPTION
===========

This page describes the D-Bus RFCOMM interface of the **bluealsad(8)** service.
The RFCOMM interface gives access to the RFCOMM terminal objects created by
this service.

Methods
-------

fd Open()
    Open RFCOMM socket for dispatching AT commands not handled internally by
    BlueALSA. This method returns a SEQPACKET socket.

    Possible Errors:
    ::

         dbus.Error.NotSupported
         dbus.Error.Failed

Properties
----------

string Transport [readonly]
    HFP/HSP transport type.

    Possible values: "HFP-AG", "HFP-HF", "HSP-AG" or "HSP-HS"

array{string} Features [readonly]
    List of features supported by the remote device.

byte Battery [readonly]
    Remote device battery level.

    Possible values: 0-100 or -1

COPYRIGHT
=========

Copyright (c) 2016-2024 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``bluealsa-rfcomm(1)``, ``bluealsad(8)``

Project web site
  https://github.com/arkq/bluez-alsa
