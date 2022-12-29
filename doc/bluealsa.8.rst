========
bluealsa
========

----------------------------
Bluetooth Audio ALSA Backend
----------------------------

:Date: January 2023
:Manual section: 8
:Manual group: System Manager's Manual
:Version: $VERSION$

SYNOPSIS
========

**bluealsa** -p *PROFILE* [*OPTION*]...

DESCRIPTION
===========

**bluealsa** is a Linux daemon to give applications access to Bluetooth audio
streams using the Bluetooth A2DP, HFP and/or HSP profiles.
It provides a D-Bus API to applications, and can be used by ALSA applications
via libasound plugins (see **bluealsa-plugins(7)** for details).

OPTIONS
=======

-h, --help
    Output a usage message and exit.

-V, --version
    Output the version number and exit.

-S, --syslog
    Send output to system logger (``syslogd(8)``).
    By default, log output is sent to stderr.

-B NAME, --dbus=NAME
    BlueALSA D-Bus service name suffix.
    Without this option, **bluealsa** registers itself as an "org.bluealsa"
    D-Bus service.  For more information see the EXAMPLES_ below.

-i hciX, --device=hciX
    HCI device to use. Can be specified multiple times to select more than one
    HCI.  Because HCI numbering can change after a system reboot, this option
    also accepts HCI MAC address for the *hciX* value, for example:
    ``--device=00:11:22:33:44:55``

    Without this option, the default is to use all available HCI devices.

-p NAME, --profile=NAME
    Enable *NAME* Bluetooth profile.
    May be given multiple number of times to enable multiple profiles.

    It is mandatory to enable at least one Bluetooth profile.
    For the list of supported profiles see Profiles_ in the **NOTES** section
    below.

-c NAME, --codec=NAME
    Enable or disable *NAME* Bluetooth audio codec.
    May be given multiple number of times to enable (or disable) multiple
    codecs.

    In order to disable given audio codec (remove it from the list of audio
    codecs enabled by default), the *NAME* has to be prefixed with **-**
    (minus) character.  It is not possible to disable SBC and CVSD codecs which
    are mandatory for A2DP and HFP/HSP respectively.

    By default BlueALSA enables SBC, AAC (if AAC support is compiled-in), CVSD
    and mSBC.
    For the list of supported audio codecs see the "Available BT audio codecs"
    section of the **bluealsa** command-line help message.

--initial-volume=NUM
    Set the initial volume to *NUM* % when a device is first connected.
    *NUM* must be an integer in the range from **0** to **100**.

    By default the volume of all PCMs of a device is set to 100% (full volume)
    when the device is first connected. For some devices, particularly
    headphones, this can lead to an unpleasant experience. This option allows
    the user to choose an alternative initial volume level. Only one value can
    be specified and each device on first connect will have the volume level of
    all its PCMs set to this value. However, a device with native volume
    control may then immediately override this level. On subsequent connects
    the volume will be set to the remembered value from the last disconnection.
    See `Volume control`_ in the **NOTES** section below for more information.

--keep-alive=SEC
    Keep Bluetooth transport alive for *SEC* number of seconds after streaming
    was closed.

    This option is required when using ``bluealsa`` with applications that
    close and then immediately re-open the same PCM as part of their
    initialization; for example applications built with the ``portaudio``
    portability library and many other "portable" applications.

    It can also be useful when playing short audio files in quick succession.
    It will reduce the gap between playbacks caused by Bluetooth audio
    transport acquisition.

--a2dp-force-mono
    Force monophonic sound for A2DP profile.

--a2dp-force-audio-cd
    Force 44.1 kHz sampling frequency for A2DP profile.
    Some Bluetooth devices can handle streams sampled at either 48kHz or
    44.1kHz, in which case they normally default to using 48kHz.
    With this option, **bluealsa** will request such a device uses only 44.1
    kHz sample rate.

--a2dp-volume
    Enable native A2DP volume control.
    By default **bluealsa** will use its own internal scaling algorithm to
    attenuate the volume.  This option disables that internal scaling and
    instead passes the volume change request to the A2DP device.
    This feature can also be controlled during runtime for individual PCMs via
    the BlueALSA D-Bus API or by the BlueALSA ALSA plugins; and if so the
    changed setting will be remembered. See `Volume control`_ in the **NOTES**
    section below for more information.
    Note that this feature might not work with all Bluetooth headsets.

--sbc-quality=MODE
    Set SBC encoder quality.
    Default value is **high**.

    The *MODE* can be one of:

    - **low** - low audio quality (mono: 114 kbps, stereo: 213 kbps)
    - **medium** - medium audio quality (mono: 132 kbps, stereo: 237 kbps)
    - **high** - high audio quality (mono: 198 kbps, stereo: 345 kbps)
    - **xq** - SBC Dual Channel HD (SBC XQ) (452 kbps)
    - **xq+** - SBC Dual Channel HD (SBC XQ+) (551 kbps)

--mp3-algorithm=TYPE
    Select LAME encoder internal algorithm.
    Default value is **expensive**.

    The *TYPE* can be one of:

    - **fast** - OK quality, really fast
    - **cheap** - good quality, fast
    - **expensive** - near-best quality, not too slow
    - **best** - best quality, slow

    If CPU power consumption is not an issue, one might safely select **best**
    as the algorithm type.
    Also, please note that the true quality is determined by the selected bit
    rate or used VBR quality option (**--mp3-vbr-quality**).

--mp3-vbr-quality=MODE
    Set variable bit rate (VBR) quality.
    Default value is **standard**.

    The *MODE* can be one of:

    - **low** - low audio quality (100-130 kbps)
    - **medium** - medium audio quality (140-185 kbps)
    - **standard** - standard audio quality (170-210 kbps)
    - **high** - high audio quality (190-250 kbps)
    - **extreme** - best audio quality, no low-pass filter (220-260 kbps)

--aac-afterburner
    Enables Fraunhofer AAC afterburner feature, which is a type of analysis by
    synthesis algorithm.
    This feature increases the audio quality at the cost of increased
    processing power and overall memory consumption.

--aac-bitrate=BPS
    Set the target bit rate for constant bit rate (CBR) mode or the maximum
    peak bit rate for variable bit rate (VBR) mode.
    Default value is **220000** bits per second.

--aac-latm-version=NUM
    Select LATM syntax version used for AAC audio transport.
    Default value is **1**.

    The *NUM* can be one of:

    - **0** - LATM syntax specified by ISO-IEC 14496-3 (2001), should work with
      all older BT devices
    - **1** - LATM syntax specified by ISO-IEC 14496-3 (2005), should work with
      newer BT devices

--aac-true-bps
    Enable true "bit per second" bit rate.

    A2DP AAC specification requires that for the constant bit rate (CBR) mode
    every RTP frame has the same bit rate and for the variable bit rate (VBR)
    mode the maximum peak bit rate limit is also per RTP frame.
    However, a single RTP frame does not contain a single full second of audio.
    This option enables true bit rate calculation (per second), which means
    that per RTP frame bit rate may vary even for CBR mode.
    This feature is not enabled by default, because it violates A2DP AAC
    specification.
    Enabling it should result in an enhanced audio quality, but will for sure
    produce fragmented RTP frames.
    If RTP fragmentation is not supported by used A2DP sink device (e.g.,
    headphones) one might hear clearly audible clicks in the playback audio.
    In such case, please do not enable this option.

--aac-vbr
    Prefer variable bit rate mode over constant bit rate mode.

    Please note, that this option does not necessarily mean that the variable
    bit rate (VBR) mode will be used.
    Used AAC configuration depends on a remote Bluetooth device capabilities.

--lc3plus-bitrate=BPS
    Set LC3plus encoder bit rate for constant bit rate mode (CBR) as *BPS*.
    Default value is **396800** bits per second.

--ldac-abr
    Enables LDAC adaptive bit rate, which will dynamically adjust encoder
    quality based on the connection stability.

--ldac-quality=MODE
    Specifies LDAC encoder quality.
    Default value is **standard**.

    The *MODE* can be one of:

    - **mobile** - mobile quality (44.1 kHz: 303 kbps, 48 kHz: 330 kbps)
    - **standard** - standard quality (44.1 kHz: 606 kbps, 48 kHz: 660 kbps)
    - **high** - high quality (44.1 kHz: 909 kbps, 48 kHz: 990 kbps)

--xapl-resp-name=NAME
    Set the product name send in the XAPL response message.
    By default, the name is set as "BlueALSA".
    However, some devices (reported with e.g., Sony WM-1000XM4) will not
    provide battery level notification unless the product name is set as
    "iPhone".

NOTES
=====

Profiles
--------

BlueALSA provides support for Bluetooth Advanced Audio Distribution Profile
(A2DP), Hands-Free Profile (HFP) and Headset Profile (HSP).
A2DP profile is dedicated for streaming music (i.e., stereo, 48 kHz or more
sampling frequency), while HFP and HSP for two-way voice transmission (mono, 8
kHz or 16 kHz sampling frequency).

The Bluetooth audio profiles are not peer-to-peer; they each have a source or
gateway role (a2dp-source, hfp-ag, or hsp-ag) and a sink or target role
(a2dp-sink, hfp-hf, hsp-hs). The source/gateway role is the audio player (e.g.,
mobile phone), the sink/target role is the audio renderer (e.g., headphones or
speaker). The **bluealsa** daemon can perform any combination of profiles and
roles, although it is most common to use it either as a source/gateway:

::

    bluealsa -p a2dp-source -p hfp-ag -p hsp-ag

or as a sink/target, either with oFono:

::

    bluealsa -p a2dp-sink -p hfp-ofono

or without oFono:

::

    bluealsa -p a2dp-sink -p hfp-hf -p hsp-hs

With A2DP, BlueALSA includes mandatory SBC codec and various optional codecs
like AAC, aptX, and other.
The full list of available optional codecs, which depends on selected
compilation options, will be shown with **bluealsa** command-line help message.

The list of profile *NAME*-s accepted by the ``--profile=NAME`` option:

- **a2dp-source** - Advanced Audio Source (streaming audio to connected device)
- **a2dp-sink** - Advanced Audio Sink (receiving audio from connected device)
- **hfp-ofono** - Hands-Free AG/HF handled by oFono
- **hfp-ag** - Hands-Free Audio Gateway
- **hfp-hf** - Hands-Free
- **hsp-ag** Headset Audio Gateway
- **hsp-hs** - Headset

The **hfp-ofono** is available only when **bluealsa** was compiled with oFono
support. Enabling HFP over oFono will automatically disable **hfp-hf** and
**hfp-ag**.

Bluez permits only one service to register the HSP and HFP profiles, and that
service is automatically registered with every HCI device.

For the A2DP profile, Bluez allows each HCI device to be registered to a
different service, so it is possible to have multiple instances of
**bluealsa** offering A2DP support, each with a unique service name given with
the ``--dbus=`` option, so long as they are registered to different HCI devices
using the ``--device=`` option. See the EXAMPLES_ below.

A profile connection does not immediately initiate the audio stream(s); audio
can only flow when the profile transport is "acquired". Acquisition can only be
performed by the source/gateway role. When acting as source/gateway,
**bluealsa** acquires the profile transport (i.e., initiates the audio
connection) when a client opens a PCM. When **bluealsa** is acting as target,
a client can open a PCM as soon as the profile is connected, but the audio
stream(s) will not begin until the remote source/gateway has acquired the
transport.

Volume control
--------------

The Bluetooth specifications for HFP and HSP include optional support
for volume control of the target by the gateway device. For A2DP, volume
control is optionally provided by the AVRCP profile. **bluealsa** provides a
single, consistent, abstracted interface for volume control of PCMs. This
interface can use the native Bluetooth features or alternatively **bluealsa**
also implements its own internal volume control, called "soft-volume". For A2DP
the default is to use soft-volume, but this can be overridden to use the
Bluetooth native support where available by using the ``--a2dp-volume`` command
line option. For HFP/HSP the default is to use Bluetooth native volume control.

When using soft-volume, **bluealsa** scales PCM samples before encoding, and
after decoding, and does not interact with the Bluetooth AVRCP volume property
or HFP/HSP volume control. Volume can only be modified by local clients. (Note
that Bluetooth headphones or speakers with their own volume controls will still
be able to alter their own volume, but this change will not be notified to
**bluealsa** local clients, they will only see the soft-volume setting).

When using native volume control, **bluealsa** links the PCM volume setting to
the AVRCP volume property or HFP/HSP volume control. No scaling of PCM samples
is applied. Volume can be modified by both local clients and the remote device.
Local clients will be notified of volume changes made by controls on the
remote device.

A2DP native volume control does not permit independent values for left and
right channels, so when a client sets such values **bluealsa** will set the
Bluetooth volume as the average of the two channels.

Volume level, mute status, and soft-volume selection can all be controlled for
each PCM by using the D-Bus API (or by using ALSA plugins, see
**bluealsa-plugins(7)** for more information). The current value of these
settings for each PCM is stored in the filesystem so that the device can be
disconnected and later re-connected without losing its volume settings.

When a device is connected, the volume level of its PCMs is set according to
the following criteria (highest priority first):

    #. saved value from previous connection of the device
    #. value set by the ``--initial-volume`` command line option
    #. **100%**

its mute status according to:

    #. saved value from previous connection
    #. **false**

and its soft-volume status according to:

    #. saved value from previous connection
    #. **false** for SCO (i.e., use native volume control).
    #. **false** for A2DP if the ``--a2dp-volume`` command line option is given
    #. **true** for A2DP (i.e., use soft-volume control).

When native volume control is enabled, then the remote device may also
modify the volume level after this initial setting. Mute and soft-volume are
implemented locally by the **bluealsa** daemon and cannot be modified by the
remote device.

Note that **bluealsa** relies on support from Bluez to implement native volume
control for A2DP using AVRCP, and Bluez has not always provided robust support
here. It is recommended to use Bluez release 5.65 or later to be certain that
native A2DP volume control will always be available with those devices which
provide it.

FILES
=====

/etc/dbus-1/system.d/bluealsa.conf
    BlueALSA service D-Bus policy file.
    D-Bus will deny all access to the **org.bluealsa** service (even to *root*)
    unless permission is granted by a policy file. The default file permits
    only *root* to own this service, and only members of the *audio* group to
    exchange messages with it.

/var/lib/bluealsa/*XX:XX:XX:XX:XX:XX*
    BlueALSA volume persistent state storage. Files are named after the
    bluetooth device address to which they refer.

EXAMPLES
========

Emulate Bluetooth headset with A2DP and HSP support:

::

    bluealsa -p a2dp-sink -p hsp-hs

On systems with more than one HCI device, it is possible to expose different
profiles on different HCI devices.
A system with three HCI devices might (for example) use *hci0* for an A2DP sink
service named "org.bluealsa.sink" and both *hci1* and *hci2* for an A2DP source
service named "org.bluealsa.source".
Such a setup might be created as follows:

::

    bluealsa -B sink -i hci0 -p a2dp-sink &
    bluealsa -B source -i hci1 -i hci2 -p a2dp-source &

Setup like this will also require a change to the BlueALSA D-Bus configuration
file in order to allow connection with BlueALSA services with suffixed names.
Please add following lines to the BlueALSA D-Bus policy:

::

    ...
    <allow send_destination="org.bluealsa.sink" />
    <allow send_destination="org.bluealsa.source" />
    ...

COPYRIGHT
=========

Copyright (c) 2016-2023 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``bluealsa-aplay(1)``, ``bluealsa-cli(1)``, ``bluealsa-rfcomm(1)``,
``bluetoothctl(1)``, ``bluealsa-plugins(7)``, ``bluetoothd(8)``

Project web site
  https://github.com/Arkq/bluez-alsa
