==============
bluealsa-aplay
==============

------------------------
a simple bluealsa player
------------------------

:Date: June 2022
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

    **bluealsa-aplay** does not perform any mixing of streams. If multiple devices
    are connected it opens a new connection to the ALSA PCM device for each stream.
    Therefore the PCM *NAME* must itself allow multiple open connections and
    mix the streams together. See option **--single-audio** to change this
    behavior. Similarly, **bluealsa-aplay** does not apply any
    transformations to the stream. For this reason it is often necessary to use
    the ALSA **dmix** and **plug** plugins in the *NAME* PCM.

--pcm-buffer-time=INT
    Set the playback PCM buffer duration time to *INT* microseconds.
    The default is 500000. It is recommended to choose a buffer time that is
    an exact multiple of the period time to avoid potential issues with some
    ALSA plugins (see --pcm-period-time option below).
    ALSA may choose the nearest available alternative if the requested value is
    not supported.

    If you experience underruns on the ALSA device then a larger buffer may
    help. However, a larger buffer will also increase the latency. For reliable
    performance the buffer time should be at least 3 times the period time.

--pcm-period-time=INT
    Set the playback PCM period duration time to *INT* microseconds.
    The default is 100000.
    ALSA may choose the nearest available alternative if the requested value is
    not supported.

    The ALSA **rate** plugin, which may be invoked by **plug**, does not always
    produce the exact required effective sample rate because of rounding errors
    in the conversion between period time and period size. This can have a
    significant impact on synchronization "drift", especially with small
    period sizes, and can also result in stream underruns (if the effective
    rate is too fast) or dropped A2DP frames in the **bluealsa(8)** server (if
    the effective rate is too slow). This effect is avoided if the selected
    period time results in an exact integer number of frames for both the source
    rate (Bluetooth) and sink rate (hardware card). For example, in
    the case of Bluetooth stream sampled at 44100Hz playing to a hardware
    device that supports only 48000Hz, choosing a period time that is a
    multiple of 10000 microseconds will result in zero rounding error.
    (10000 µs at 44100Hz is 441 frames, and at 48000Hz is 480 frames).

    See also DMIX_ section below for more information on rate calculation
    rounding errors.

-M NAME, --mixer-device=NAME
    Select ALSA mixer device to use for controlling audio output mute state
    and volume level.
    In order to use this feature, BlueALSA PCM can not use software volume.
    The default is ``default``.

--mixer-name=NAME
    Set the name of the ALSA simple mixer control to use.
    The default is ``Master``.

    To work with ``bluealsa-aplay`` this simple control must provide decibel
    scaling information for the volume control. Most, but not all, modern sound
    cards do provide this information.

--mixer-index=NUM
    Set the index of the ALSA simple mixer control.
    The default is ``0``.

    This is required only if the simple mixer control name applies to multiple
    simple controls on the same card. This is most common with HDMI devices
    which may have many playback ports.

--profile-a2dp
    Use A2DP profile (default).

--profile-sco
    Use SCO profile.

    Note: Only one of A2DP or SCO can be used. If both are specified, the
    last one given will be selected.

--single-audio
    Allow only one Bluetooth device to play audio at a time.
    If multiple devices are connected, only the first to start will play, the
    others will be paused. When that first device stops, then the next to send
    audio will be played.

    Without this option, **bluealsa-aplay** plays audio from all selected
    Bluetooth devices.
    Please note that playing from all Bluetooth devices at a time requires used
    PCM to be able to mix audio from multiple sources (i.e., it can be opened
    more than once; for example the ALSA **dmix** plugin).

DMIX
====

The ALSA `dmix` plugin will ignore the period and buffer times selected by the
application (because it has to allow connections from multiple applications).
Instead it will choose its own values, which can lead to rounding errors in the
period size calculation when used with the ALSA `rate` plugin. To avoid this, it
is recommended to explicitly define the hardware period size and buffer size for
dmix in your ALSA configuration. For example, suppose we want a period time of
100000 µs and a buffer holding 5 periods with an Intel 'PCH' card:

::

    defaults.dmix.PCH.period_time 100000
    defaults.dmix.PCH.periods 5

Alternatively we can define a PCM with the required setting:

::

    pcm.dmix_rate_fix {
        type plug
        slave.pcm {
            type dmix
            ipc_key 12345
            slave {
                pcm "hw:0,0"
                period_time 100000
                periods 5
            }
        }
    }

SEE ALSO
========

``bluealsa(8)``, ``bluealsa-rfcomm(1)``

Project web site at https://github.com/Arkq/bluez-alsa

COPYRIGHT
=========

Copyright (c) 2016-2021 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.
