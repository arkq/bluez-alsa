===============
bluealsa-rfcomm
===============

-------------------------------------
a simple RFCOMM terminal for BlueALSA
-------------------------------------

:Date: August 2024
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**bluealsa-rfcomm** [*OPTION*]... *DEVICE-PATH*

DESCRIPTION
===========

**bluealsa-rfcomm** provides access to HSP/HFP RFCOMM terminal for connected
Bluetooth device specified by the *DEVICE-PATH*. The *DEVICE-PATH* can be
either a Bluetooth device D-Bus path defined by BlueZ (org.bluez) or BlueALSA
(org.bluealsa) service.

OPTIONS
=======

-h, --help
    Output a usage message and exit.

-V, --version
    Output the version number and exit.

-B NAME, --dbus=NAME
    BlueALSA service name suffix. For more information see ``--dbus``
    option of ``bluealsad(8)`` service daemon.

-p, --properties
    Print the properties of the given *DEVICE-PATH*

EXAMPLES
========

::

    bluealsa-rfcomm /org/bluealsa/hci0/dev_1C_48_F9_9D_81_5C
    1C:48:F9:9D:81:5C> RING
    > AT+IPHONEACCEV=2,1,6,2,0
    > AT+CKPD=200
    disconnected

COPYRIGHT
=========

Copyright (c) 2016-2024 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``bluealsa-aplay(1)`` ``bluealsad(8)``

Project web site
  https://github.com/arkq/bluez-alsa
