==============
bluealsa-aplay
==============

------------------------
a simple bluealsa player
------------------------

:Date: August 2020
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**bluealsa-aplay** [*OPTION*]... [*BT-ADDR*]...

DESCRIPTION
===========

Capture audio streams from Bluetooth devices (via ``bluealsa(8)``) and play them to an ALSA
playback device.

By default **bluealsa-aplay** captures audio from all connected Bluetooth devices.
It is possible to select specific Bluetooth devices by providing a list of *BT-ADDR* MAC
addresses.
Using the special MAC address **00:00:00:00:00:00** will disable device filtering - the
same as the default behavior.

OPTIONS
=======

-h, --help
    Output a usage message and exit.

-V, --version
    Output the version number and exit.

-v, --verbose
    Make the output more verbose.

-l, --list-devices
    List connected Bluetooth audio devices.

-L, --list-pcms
    List available Bluetooth audio PCMs on connected devices.

-B NAME, --dbus=NAME
    BlueALSA service name suffix.
    For more information see ``--dbus=NAME`` option of ``bluealsa(8)`` service daemon.

-D NAME, --pcm=NAME
    Select ALSA playback PCM device to use for audio output.
    The default is ``default``.

--pcm-buffer-time=INT
    Set the playback PCM buffer duration time to *INT* microseconds.
    The default is 500000.
    ALSA may choose the nearest available alternative if the requested value is
    not supported.

--pcm-period-time=INT
    Set the playback PCM period duration time to *INT* microseconds.
    The default is 100000.
    ALSA may choose the nearest available alternative if the requested value is
    not supported.

--profile-a2dp
    Use A2DP profile (default).

--profile-sco
    Use SCO profile.

--single-audio
    Allow only one Bluetooth device to play audio at a time.
    Without this option, **bluealsa-aplay** plays audio from all selected Bluetooth devices.
    Please note that playing from all Bluetooth devices at a time requires used PCM to be able
    to mix audio from multiple sources (i.e., it can be opened more than once; for
    example the ALSA **dmix** plugin).

SEE ALSO
========

``bluealsa(8)``, ``bluealsa-rfcomm(1)``

Project web site at https://github.com/Arkq/bluez-alsa

COPYRIGHT
=========

Copyright (c) 2016-2020 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.
