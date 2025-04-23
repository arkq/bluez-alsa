==============
bluealsa-aplay
==============

------------------------
a simple BlueALSA player
------------------------

:Date: April 2025
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**bluealsa-aplay** [*OPTION*]... [*BT-ADDR*]...

DESCRIPTION
===========

Capture audio streams from Bluetooth devices (via ``bluealsad(8)``) and play
them to an ALSA playback device. Optionally, Bluetooth AVRCP volume control
requests are applied to a local mixer control to permit remote volume
control.

By default **bluealsa-aplay** captures audio from all connected Bluetooth
devices.  It is possible to select specific Bluetooth devices by providing a
list of *BT-ADDR* MAC addresses.
Using the special MAC address **00:00:00:00:00:00** will disable device
filtering - the same as the default behavior.

If built with **libsamplerate** support **bluealsa-aplay** can compensate for
timer drift by using adaptive rate resampling.

**bluealsa-aplay** can also be used to list connected Bluetooth audio devices
and PCMs.

OPTIONS
=======

-h, --help
    Output a usage message and exit.

-V, --version
    Output the version number and exit.

-S, --syslog
    Send output to system logger (``syslogd(8)``).
    By default, log output is sent to stderr.

--loglevel=LEVEL
    Set the priority level threshold for log messages. Only messages of the
    given level or higher are logged. The *LEVELs* are, in decreasing order:

    - **error** - error conditions
    - **warning** - warning conditions
    - **info** - informational messages

    If **bluealsa-aplay** was built with debug enabled, then an additional,
    lowest, level is  available:

    - **debug** - debug messages

    If this option is not given then the default is to use the lowest level
    (i.e., all messages are logged).

-v, --verbose
    Make the output more verbose. This option may be given multiple times to
    increase the verbosity.

-l, --list-devices
    List connected Bluetooth audio devices.

-L, --list-pcms
    List available Bluetooth audio PCMs on connected devices.

-B NAME, --dbus=NAME
    BlueALSA service name suffix.
    For more information see ``--dbus=NAME`` option of ``bluealsad(8)`` service
    daemon.

-D NAME, --pcm=NAME
    Select ALSA playback PCM device to use for audio output.
    The default is ``default``.

    Internally, **bluealsa-aplay** is able to perform sample rate conversion
    if it was built with libsamplerate support (see the option
    **--resampler=**), but does not perform any other audio transformations
    nor streams mixing. If multiple Bluetooth devices are connected it simply
    opens a new connection to the ALSA PCM device for each stream. Selected
    hardware parameters like sample format and number of channels are
    taken from the audio profile of a particular Bluetooth connection. Note,
    that each connection can have a different setup.

    If playing multiple streams at the same time is not desired, it is possible
    to change that behavior by using the **--single-audio** option.

    For more information see the EXAMPLES_ section below.

--pcm-buffer-time=INT
    Set the playback PCM buffer duration time to *INT* microseconds.
    The default is four times the period time. It is recommended to choose a
    buffer time that is an exact multiple of the period time to avoid potential
    issues with some ALSA plugins (see --pcm-period-time option below). For
    reliable performance the buffer time should be at least 3 times the period
    time.

    ALSA may choose the nearest available alternative if the requested value is
    not supported; and some ALSA devices may ignore the requested value
    completely (e.g. **dmix**, see dmix_ in the **NOTES** section below).

--pcm-period-time=INT
    Set the playback PCM period duration time to *INT* microseconds. The
    default is 50000 for A2DP and 20000 for SCO profiles.
    ALSA may choose the nearest available alternative if the requested value is
    not supported; and some ALSA devices may ignore the requested value
    completely (e.g. **dmix**, see dmix_ in the **NOTES** section below).

    If you experience underruns on the ALSA device or overruns on the Bluetooth
    stream then a larger period time may help. However, a larger period time
    will also increase the latency.

    The ALSA **rate** plugin, which may be invoked by **plug**, does not always
    produce the exact required effective sample rate because of rounding errors
    in the conversion between period time and period size. This can have a
    significant impact on synchronization "drift", especially with small period
    sizes, and can also result in stream underruns (if the effective rate is
    too fast) or dropped frames (if the effective rate is too slow). This
    effect is avoided if the selected period time results in an exact integer
    number of frames for both the source rate (Bluetooth) and sink rate
    (hardware card). For example, in the case of Bluetooth stream sampled at
    44100 Hz playing to a hardware device that supports only 48000 Hz, choosing
    a period time that is a multiple of 10000 microseconds will result in zero
    rounding error. (10000 µs at 44100 Hz is 441 frames, and at 48000 Hz is 480
    frames).

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
    Note that **bluealsa-aplay** does not assume that the mixer device has the
    same name, or is on the same card, as the PCM device. If this option is not
    given then it will use ``default`` as the mixer device no matter which name
    is given with the **--pcm=** option.

--mixer-control=NAME
    Set the name of the ALSA simple mixer control to use.
    The default is ``Master``.

    If this simple control provides decibel scaling information then
    **bluealsa-aplay** uses that information when converting the Bluetooth
    volume level to or from a value for the control. When no decibel scaling
    information is available then **bluealsa-aplay** assumes that the control
    has a linear scale.
    Most, but not all, modern sound cards do provide decibel scaling
    information.

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

--resampler=METHOD
    Use libsamplerate to convert the stream from the Bluetooth sample rate to
    the ALSA PCM sample rate. This option is only available if
    **bluealsa-aplay** was built with libsamplerate support. The resampler uses
    adaptive resampling to compensate for timer drift between the Bluetooth
    timer and the ALSA device timer. Resampling can be CPU intensive and
    therefore by default this option is not enabled. *METHOD* specifies which
    libsamplerate converter to use, and may be one of 6 values:

    - **sinc-best** - use the SRC_SINC_BEST_QUALITY converter; generates the
      highest quality output but also has very high CPU usage.

    - **sinc-medium** - use the SRC_SINC_MEDIUM_QUALITY converter; generates
      high quality output and has moderately high CPU usage.

    - **sinc-fastest** - use the SRC_SINC_FASTEST converter; generates good
      quality output with lower CPU usage than the other SINC based converters.
      Often this converter is the best compromise for Bluetooth audio.

    - **linear** - use the SRC_LINEAR converter. The audio quality is
      relatively poor compared to the SINC converters. Quality and CPU usage
      is similar to the ALSA rate plugin's own internal linear converter, but
      this option also compensates for timer drift, which is not possible with
      the ALSA rate plugin.

    - **zero-order-hold** - use the SRC_ZERO_ORDER_HOLD converter; the lowest
      quality converter of libsamplerate. CPU usage is very low so may be
      better suited to very low power embedded processors.

    - **none** - do not perform any resampling; the ALSA PCM device is then
      responsible for rate conversion, and no timer drift adjustment is made.
      This is the default when no resampler is specified.

    See `Delay, timer drift, and resampling`_ in the **NOTES** section below
    for more information.

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
adjustment will have been applied to the PCM stream within the **bluealsad**
daemon; so **bluealsa-aplay** does not operate the mixer control in this case.

When using ``--volume=none`` or ``--volume=software``, then the mixer options
``--mixer-device``, ``--mixer-control`` and ``--mixer-index`` are ignored, and
**bluealsa-aplay** will not operate any mixer controls, even if some other
application changes the PCM volume mode to native while in use.

When native volume control is enabled (using either ``--volume=auto`` or
``--volume=mixer``) then the ALSA mixer control will be operated only when the
PCM stream is active (i.e., the remote device is sending audio). If a
connected remote device requests a volume change when no active stream is
playing, then **bluealsa-aplay** will ignore that request.
When the audio stream starts then **bluealsa-aplay** will change the Bluetooth
volume to match the current setting of the ALSA mixer control.

Native Bluetooth volume control for A2DP relies on AVRCP volume control in
BlueZ, which has not always been reliably implemented. It is recommended to use
BlueZ release 5.65 or later to be certain that native A2DP volume control will
always be available with those devices which provide it.

See ``bluealsad(8)`` for more information on native and soft-volume volume
control.

Delay, timer drift, and resampling
----------------------------------

When using A2DP, **bluealsa-aplay** reports the current stream delay back to
the source device to permit A/V synchronization (requires BlueZ 5.79 or later).
The delay is influenced by a number of factors, but the largest single
contributor is buffering within **bluealsa-aplay**. Some buffering is essential
to maintain a steady audio stream given that codec decoding and radio
interference can cause large variations in the timing of audio sample delivery.

By default, **bluealsa-aplay** uses a period time of 50ms for A2DP which
results in a typical delay of between 160ms and 210ms. The delay is three times
the period time plus an additional amount which depends on the codec and the
ALSA device.
It is possible to modify the period time using the command-line option
``--pcm-period-time`` and this will directly affect the resulting delay.

In poor radio reception conditions, audio samples may be "lost" and then be
re-sent by the source device. This can cause a long break in the stream
followed by a sudden "flood" of samples. A larger delay will give
**bluealsa-aplay** greater capacity to handle such breaks without interrupting
the playback stream. However, many devices are unable to synchronize A/V when
the audio delay is too high. The limit depends very much on the source device,
and also the application running on that device, but as a general guide it is
best to keep the delay below about 400ms, and therefore to keep the period time
below about 100ms.

Real-world timers never "tick" at *exactly* the same rate, and so the ALSA
device will not consume audio samples at *exactly* the same rate as the
Bluetooth device produces them. This difference is known as "timer drift" and
causes the **bluealsa-aplay** buffer to either slowly run empty or to slowly
become too full to receive samples from the remote device. If the buffer
becomes empty **bluealsa-aplay** will stop the ALSA device briefly to allow
time for more samples to arrive; if the buffer becomes full then
**bluealsa-aplay** will drop samples to create space. In either case there
will be a noticeable "blip" in the resulting audio.

In most cases the timer drift is very small and in good radio reception
conditions **bluealsa-aplay** is able to play a continuous stream for several
hours without any "blips".

There are some ALSA devices for which the timer drift can be more significant
(for example, see the note on dmix_ below); or instances where it is
necessary to maintain a steady audio stream for longer. To help with these
cases **bluealsa-aplay** can perform adaptive resampling which dynamically
makes small adjustments to the sample rate to compensate for timer drift,
allowing a continuous stream to be maintained for much longer. (This feature
is only available if **bluealsa-aplay** was built with **libsamplerate**
support). Five different sample rate conversion algorithms are offered, each
offering a different compromise between audio quality and CPU load. Note that
the three SINC based converters output bandwidth limited audio, guaranteeing
that the highest frequency component of the output is less than half of the
output sample rate. The linear and zero-order-hold converters are *not*
bandwidth limited, and therefore their output may contain some higher frequency
components. These higher frequencies may result in "aliasing" effects created
by the DAC which can result in noticeable degradation of the audio quality.
In practice many sound cards do appear to apply a low-pass filter
before the DAC which prevents these aliasing effects; however it is
recommended to only use these two converters if the host CPU lacks sufficient
processing power to use the sinc-fastest converter.

dmix
----

The ALSA **dmix** plugin will ignore the period and buffer times selected by
the application (because it has to allow connections from multiple
applications). Instead it will choose its own values, which can lead to
rounding errors in the period size calculation when used with the ALSA **rate**
plugin (but not when using the *--resampler=* option). To avoid this, it is
recommended to explicitly define the hardware period size and buffer size for
**dmix** in your ALSA configuration. For example, suppose we want a period time
of 50000 µs and a buffer holding 4 periods with an Intel 'PCH' card:

::

    defaults.dmix.PCH.period_time 50000
    defaults.dmix.PCH.periods 4

Alternatively we can define a PCM with the required setting:

::

    pcm.dmix_rate_fix {
        type plug
        slave.pcm {
            type dmix
            ipc_key 12345
            slave {
                pcm "hw:0,0"
                period_time 50000
                periods 4
            }
        }
    }

EXAMPLES
========

The simplest usage of **bluealsa-aplay** is to run it with no arguments. It
will play audio from all connected Bluetooth devices to the **default** ALSA
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
    bluealsa-aplay --pcm=default:USB --mixer-device=hw:USB --mixer-control=Speaker C8:F7:33:66:F0:DE &

Such setup will route ``94:B8:6D:AF:CD:EF`` and ``F8:87:F1:B8:30:85`` Bluetooth
devices to the ``default`` ALSA playback PCM device and ``C8:F7:33:66:F0:DE``
device to the USB sound card. For the USB sound card the ``Speaker`` control
element will be used as a hardware volume control knob.

COPYRIGHT
=========

Copyright (c) 2016-2025 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``amixer(1)``, ``aplay(1)``, ``bluealsa-rfcomm(1)``, ``bluealsad(8)``

Project web site
  https://github.com/arkq/bluez-alsa
