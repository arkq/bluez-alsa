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

    See the section `Selecting An ALSA Playback PCM`_ below for more information
    on ALSA playback PCM devices.

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

Selecting An ALSA Playback PCM
==============================

**bluealsa-aplay** does not apply any transformations to the audio stream.
For this reason it is often necessary to use the ALSA **plug** plugin in the
ALSA playback PCM.

**bluealsa-aplay** does not perform any mixing of streams. If multiple bluetooth
devices are connected it opens a new connection to the ALSA PCM device for each
stream. Therefore the ALSA playback PCM must itself allow multiple open
connections and mix the streams together (see option **--single-audio** to
change this behavior). For this reason it is often necessary to use the ALSA
**dmix** plugin in the ALSA playback PCM.

For most distributions, the installed definition of ``default`` for each sound
card includes both ``dmix`` and ``plug`` where necessary, so is generally the
best choice unless there is some specific reason to prefer some other device.
If there is more than one sound card attached then the appropriate one can be
selected with:

::

    bluealsa-aplay -D default:CARDNAME

A list of attached sound card names can be obtained using **aplay -l**, for
example:

::

    $ aplay -l
    **** List of PLAYBACK Hardware Devices ****
    card 0: PCH [HDA Intel PCH], device 0: ALC236 Analog [ALC236 Analog]
      Subdevices: 1/1
      Subdevice #0: subdevice #0
    card 1: Device [USB Audio Device], device 0: USB Audio [USB Audio]
      Subdevices: 1/1
      Subdevice #0: subdevice #0

Here the first word after the card number is the card name, so that to use the
HDA Intel PCH card:

::

    bluealsa-aplay -D default:PCH

and to use the USB card:

::

    bluealsa-aplay -D default:Device


Some sound cards offer more than one playback PCM device. ALSA identifies these
devices by using an index number for each. Such a card might produce **aplay -l**
output like:

::

    $ aplay -l
    **** List of PLAYBACK Hardware Devices ****
    card 0: PCH [HDA Intel PCH], device 0: ALC236 Analog [ALC236 Analog]
      Subdevices: 1/1
      Subdevice #0: subdevice #0
    card 0: PCH [HDA Intel PCH], device 3: HDMI 0 [HDMI 0]
      Subdevices: 1/1
      Subdevice #0: subdevice #0
    card 0: PCH [HDA Intel PCH], device 7: HDMI 1 [HDMI 1]
      Subdevices: 1/1
      Subdevice #0: subdevice #0

Here the device index number is given by ``device N``, so in this example
the Analog output is used with:

::

    bluealsa-aplay -D default:PCH,0

and the first HDMI port (``HDMI 0``) is used with:

::

    bluealsa-aplay -D default:PCH,3

If the device index is not specified, the default is ``0``.

There will be some cases where the ``default`` device is not sufficient. These
are generally special cases, such as selecting a specific subset of channels
from a multi-channel device or to duplicate the stream across multiple output
devices etc. These cases generally require some additional configuration by the
user, and it is recommended to seek advice from your distribution as this can be
quite complex and is beyond the scope of this manual.

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

COPYRIGHT
=========

Copyright (c) 2016-2022 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``amixer(1)``, ``aplay(1)``, ``bluealsa(8)``, ``bluealsa-rfcomm(1)``

Project web site
  https://github.com/Arkq/bluez-alsa
