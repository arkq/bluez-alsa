==============
bluealsa-aplay
==============

------------------------
a simple bluealsa player
------------------------

:Date: September 2023
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**bluealsa-aplay** [*OPTION*]... [*BT-ADDR*]...

DESCRIPTION
===========

Capture audio streams from Bluetooth devices (via ``bluealsa(8)``) and play
them to an ALSA playback device.

By default **bluealsa-aplay** captures audio from all connected Bluetooth
devices.  It is possible to select specific Bluetooth devices by providing a
list of *BT-ADDR* MAC addresses.
Using the special MAC address **00:00:00:00:00:00** will disable device
filtering - the same as the default behavior.

OPTIONS
=======

-h, --help
    Output a usage message and exit.

-V, --version
    Output the version number and exit.

-S, --syslog
    Send output to system logger (``syslogd(8)``).
    By default, log output is sent to stderr.

-v, --verbose
    Make the output more verbose.

-l, --list-devices
    List connected Bluetooth audio devices.

-L, --list-pcms
    List available Bluetooth audio PCMs on connected devices.

-B NAME, --dbus=NAME
    BlueALSA service name suffix.
    For more information see ``--dbus=NAME`` option of ``bluealsa(8)`` service
    daemon.

-D NAME, --pcm=NAME
    Select ALSA playback PCM device to use for audio output.
    The default is ``default``.

    Internally, **bluealsa-aplay** does not perform any audio transformations
    nor streams mixing. If multiple Bluetooth devices are connected it simply
    opens a new connection to the ALSA PCM device for each stream. Selected
    hardware parameters like sampling frequency and number of channels are
    taken from the audio profile of a particular Bluetooth connection. Note,
    that each connection can have a different setup.

    If playing multiple streams at the same time is not desired, it is possible
    to change that behavior by using the **--single-audio** option.

    For more information see the EXAMPLES_ section below.

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
    significant impact on synchronization "drift", especially with small period
    sizes, and can also result in stream underruns (if the effective rate is
    too fast) or dropped A2DP frames in the **bluealsa(8)** server (if the
    effective rate is too slow). This effect is avoided if the selected period
    time results in an exact integer number of frames for both the source rate
    (Bluetooth) and sink rate (hardware card). For example, in the case of
    Bluetooth stream sampled at 44100Hz playing to a hardware device that
    supports only 48000Hz, choosing a period time that is a multiple of 10000
    microseconds will result in zero rounding error.  (10000 µs at 44100Hz is
    441 frames, and at 48000Hz is 480 frames).

    See also dmix_ in the **NOTES** section below for more information on
    rate calculation rounding errors.

--volume=TYPE
    Select the desired method of implementing remote volume control. *TYPE* may
    be one of four values:

    - **auto** - the volume control method is determined by the BlueALSA PCM.
      This is the default when this option is not given. **bluealsa-aplay**
      operates its configured ALSA mixer control to apply volume change
      requests received from the remote Bluetooth device if and only if the PCM
      is using native ("pass-through") volume control.

    - **mixer** - **bluealsa-aplay** will force the BlueALSA PCM volume mode
      setting to native ("pass-through") before starting the PCM stream, and
      then operate the same as for **auto** above.

    - **none** - **bluealsa-aplay** will force the BlueALSA PCM volume mode
      setting to native ("pass-through") before starting the PCM
      stream.  It will not operate its configured ALSA mixer. This can be used
      to effectively disable remote volume control; or it can be used to allow
      some other application to apply remote volume change requests.

    - **software** - **bluealsa-aplay** will force the BlueALSA PCM volume mode
      setting to soft-volume ("software") and then will not operate its
      configured ALSA mixer. This can be used to enable remote volume control
      without using an ALSA mixer.

    See `Volume control`_ in the **NOTES** section below for more information
    on volume control.

-M NAME, --mixer-device=NAME
    Select ALSA mixer device to use for controlling audio output mute state
    and volume level.
    In order to use this feature, BlueALSA PCM can not use software volume.
    The default is ``default``.

--mixer-name=NAME
    Set the name of the ALSA simple mixer control to use.
    The default is ``Master``.

    To work with **bluealsa-aplay** this simple control must provide decibel
    scaling information for the volume control. Most, but not all, modern sound
    cards do provide this information.

--mixer-index=NUM
    Set the index of the ALSA simple mixer control.
    The default is ``0``.

    This is required only if the simple mixer control name applies to multiple
    simple controls on the same card. This is most common with HDMI devices
    for which the index indicates the controlled HDMI PCM device.

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

NOTES
=====

Volume control
--------------

If the Bluetooth PCM is using BlueALSA soft-volume volume control, then volume
adjustment will have been applied to the PCM stream within the **bluealsa**
daemon; so **bluealsa-aplay** does not operate the mixer control in this case.

When using ``--volume=none`` or ``--volume=software``, then the mixer options
``--mixer-device``, ``--mixer-name`` and ``--mixer-index`` are ignored, and
**bluealsa-aplay** will not operate any mixer controls, even if some other
application changes the PCM volume mode to native while in use.

When using ``--volume=auto`` or ``--volume=mixer`` the ALSA mixer control will
be operated only when the PCM stream is active, (i.e., the remote device is
sending audio). If a connected remote device requests a volume change when no
active stream is playing, then **bluealsa-aplay** will ignore that request.
When the audio stream starts then **bluealsa-aplay** will change the Bluetooth
volume to match the current setting of the ALSA mixer control.

Native Bluetooth volume control for A2DP relies on AVRCP volume control in
BlueZ, which has not always been reliably implemented. It is recommended to use
BlueZ release 5.65 or later to be certain that native A2DP volume control will
always be available with those devices which provide it.

See ``bluealsa(8)`` for more information on native and soft-volume volume
control.

dmix
----

The ALSA `dmix` plugin will ignore the period and buffer times selected by the
application (because it has to allow connections from multiple applications).
Instead it will choose its own values, which can lead to rounding errors in the
period size calculation when used with the ALSA `rate` plugin. To avoid this,
it is recommended to explicitly define the hardware period size and buffer size
for dmix in your ALSA configuration. For example, suppose we want a period time
of 100000 µs and a buffer holding 5 periods with an Intel 'PCH' card:

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

EXAMPLES
========

The simplest usage of **bluealsa-aplay** is to run it with no arguments. It
will play audio from all connected Bluetooth devices to the ``default`` ALSA
playback PCM.

::

    bluealsa-aplay

If there is more than one sound card attached one can create a setup where the
audio of a particular Bluetooth device is played to a specific sound card. The
setup below shows how to do this using the ``--pcm=NAME`` option and known
Bluetooth device addresses.

Please note that in the following example we assume that the second card is
named "USB" and the appropriate mixer control is named "Speaker". Real names
of attached sound cards can be obtained by running **aplay -l**. A list of
control names for a card called "USB" can be obtained by running
**amixer -c USB scontrols**.

::

    bluealsa-aplay --pcm=default 94:B8:6D:AF:CD:EF F8:87:F1:B8:30:85 &
    bluealsa-aplay --pcm=default:USB C8:F7:33:66:F0:DE &

Also, it might be desired to specify ALSA mixer device and/or control element
for each ALSA playback PCM device. This will be mostly useful when BlueALSA PCM
does not use software volume (for more information see ``--volume`` option
above).

::

    bluealsa-aplay --pcm=default 94:B8:6D:AF:CD:EF F8:87:F1:B8:30:85 &
    bluealsa-aplay --pcm=default:USB --mixer-device=hw:USB --mixer-name=Speaker C8:F7:33:66:F0:DE &

Such setup will route ``94:B8:6D:AF:CD:EF`` and ``F8:87:F1:B8:30:85`` Bluetooth
devices to the ``default`` ALSA playback PCM device and ``C8:F7:33:66:F0:DE``
device to the USB sound card. For the USB sound card the ``Speaker`` control
element will be used as a hardware volume control knob.

COPYRIGHT
=========

Copyright (c) 2016-2023 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``amixer(1)``, ``aplay(1)``, ``bluealsa-rfcomm(1)``, ``bluealsa(8)``

Project web site
  https://github.com/arkq/bluez-alsa
