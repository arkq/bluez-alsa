=================
org.bluealsa.PCM1
=================

-----------------------------
Bluetooth Audio PCM D-Bus API
-----------------------------

:Date: September 2024
:Manual section: 7
:Manual group: D-Bus Interface
:Version: $VERSION$

SYNOPSIS
========

:Service:     org.bluealsa[.unique ID]
:Interface:   org.bluealsa.PCM1
:Object path: [variable prefix]/{hci0,...}/dev_XX_XX_XX_XX_XX_XX/[type]/[mode]

DESCRIPTION
===========

This page describes the D-Bus PCM interface of the **bluealsad(8)** service.
The PCM interface gives access to individual PCM objects created by this
service.

Methods
-------

fd, fd Open()
    Open BlueALSA PCM stream. This method returns two file descriptors,
    respectively PCM stream PIPE and PCM controller SEQPACKET socket.

    Controller socket commands: "Drain", "Drop", "Pause", "Resume"

    Possible Errors:
    ::

        dbus.Error.InvalidArguments
        dbus.Error.NotSupported
        dbus.Error.Failed

array{string, dict} GetCodecs()
    Return the array of additional PCM codecs. Client can switch to one of
    these codecs with the SelectCodec() D-Bus method call.

    The dictionary may contain the following properties:

    :array{byte} Capabilities:
        A2DP codec capabilities blob.
    :array{byte} Channels:
        List of supported channel counts.
    :array{array{string}} ChannelMaps:
        List of supported channel maps.
    :array{uint32} Rates:
        List of supported sample rates.

void SelectCodec(string codec, dict props)
    Select PCM codec. This call shall be made before PCM stream opening for
    given transport type, otherwise the ongoing stream (or PCM counterpart:
    sink, source) will be terminated.

    For A2DP codecs, client can override built-in logic for selecting codec
    configuration by providing the configuration blob via the "Configuration"
    property. Provided configuration must be valid for given codec in respect
    to BlueALSA and peer device capabilities. Otherwise, the call will fail.
    It is possible to override this validation by setting the "NonConformant"
    property to true.

    In case of codecs which support different number of audio channels or
    sample rates, client can select the desired configuration by providing the
    "Channels" and "Rate" properties respectively. These properties take
    precedence over the provided codec configuration.

    Possible Errors:
    ::

        dbus.Error.InvalidArguments
        dbus.Error.NotSupported
        dbus.Error.Failed

Properties
----------

object Device [readonly]
    BlueZ device object path.

uint32 Sequence [readonly]
    This property indicates the sequence in which devices connected. The larger
    the value, the later the device was connected.

string Transport [readonly]
    Underlying Bluetooth transport type.

    Possible values:
    ::

        "A2DP-sink"
        "A2DP-source"
        "HFP-AG"
        "HFP-HF"
        "HSP-AG"
        "HSP-HS"

string Mode [readonly]
    PCM stream operation mode (direction).

    Possible values:
    ::

        "sink"
        "source"

boolean Running [readonly]
    This property is true when the Bluetooth transport for this PCM is
    acquired and able to transfer audio samples.

uint16 Format [readonly]
    Stream format identifier. The highest two bits of the 16-bit identifier
    determine the signedness and the endianness. Next 6 bits determine the
    physical width of a sample in bytes. The lowest 8 bits are used to store
    the actual sample bit-width.

    Examples:
    ::

        0x4210 - unsigned 16-bit 2 bytes big-endian
        0x8418 - signed 24-bit 4 bytes little-endian

byte Channels [readonly]
    Number of audio channels.

array{string} ChannelMap [readonly]
    Channel map for selected codec.

uint32 Rate [readonly]
    Sample rate in Hz.

string Codec [readonly]
    Bluetooth transport codec. This property is available only when transport
    codec is selected.

array{byte} CodecConfiguration [readonly]
    Optional. Bluetooth transport codec configuration blob. This property is
    available only for transports which support codec configuration
    (e.g. A2DP).

uint16 Delay [readonly]
    Approximate PCM delay in 1/10 of millisecond.

int16 ClientDelay [readwrite]
    Positive (or negative) client side delay in 1/10 of millisecond.

    This property shall be set by the client in order to account for the client
    side delay. In case of PCM source it shall be set to a value reported by a
    playback subsystem to account for playback delay. In case of PCM sink it
    can be used to adjust the Delay property to compensate for devices that do
    not report accurate delay values.

boolean SoftVolume [readwrite]
    This property determines whether BlueALSA will make volume control
    internally or will delegate this task to BlueALSA PCM client or connected
    Bluetooth device respectively for PCM sink or PCM source.

array{byte} Volume [readwrite]
    This property holds volume (loudness) for all channels. The highest bit
    of each byte determines whether channel is muted. The order of channels
    is defined by the ChannelMap property.

    Possible values:
    ::

       A2DP: 0-127
       SCO:  0-15

COPYRIGHT
=========

Copyright (c) 2016-2024 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``bluealsactl(1)``, ``bluealsa-plugins(5)``, ``bluealsad(8)``

Project web site
  https://github.com/arkq/bluez-alsa
