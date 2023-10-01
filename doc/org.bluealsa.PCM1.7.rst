=================
org.bluealsa.PCM1
=================

---------------------------------
Bluetooth Audio PCM D-Bus API
---------------------------------

:Date: October 2023
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

This page describes the D-Bus PCM interface of the **bluealsa(8)** service.
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

void SelectCodec(string codec, dict props)
    Select PCM codec. This call shall be made before PCM stream opening for
    given transport type, otherwise the ongoing stream (or PCM counterpart:
    sink, source) will be terminated.

    For A2DP codecs, client can override built-in logic for selecting codec
    configuration by providing the configuration blob via the "Configuration"
    property.

    Possible Errors:
    ::

        dbus.Error.InvalidArguments
        dbus.Error.NotSupported
        dbus.Error.Failed

void SetDelayAdjustment(string codec, int16 adjustment)
    Set an arbitrary adjustment (+/-) to the reported Delay in 1/10 of
    millisecond for a specific codec. This adjustment is applied to the Delay
    property when that codec is selected, and can be used to compensate for
    devices that do not report accurate Delay values.

    Possible Errors:
    ::

        dbus.Error.InvalidArguments

array{string, int16} GetDelayAdjustments()
    Return the array of currently set delay adjustments. Each entry of the
    array gives the name of a codec and the adjustment that the PCM will apply
    to the Delay property when that codec is selected.

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

uint32 Sampling [readonly]
    Sampling frequency.

string Codec [readonly]
    Bluetooth transport codec. This property is available only when transport
    codec is selected.

array{byte} CodecConfiguration [readonly]
    Optional. Bluetooth transport codec configuration blob. This property is
    available only for transports which support codec configuration
    (e.g. A2DP).

uint16 Delay [readonly]
    Approximate PCM delay in 1/10 of millisecond.

int16 DelayAdjustment [readonly]
    An adjustment (+/-) included within the reported Delay in 1/10 of
    millisecond to compensate for devices that do not report accurate delay
    values.

boolean SoftVolume [readwrite]
    This property determines whether BlueALSA will make volume control
    internally or will delegate this task to BlueALSA PCM client or connected
    Bluetooth device respectively for PCM sink or PCM source.

uint16 Volume [readwrite]
    This property holds volume (loudness) value and mute information for
    channel 1 (left) and 2 (right). Data for channel 1 is stored in the upper
    byte, channel 2 is stored in the lower byte. The highest bit of both bytes
    determines whether channel is muted.

    Possible values:
    ::

       A2DP: 0-127
       SCO:  0-15

COPYRIGHT
=========

Copyright (c) 2016-2023 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``bluealsa-cli(1)``, ``bluealsa-plugins(5)``, ``bluealsa(8)``

Project web site
  https://github.com/arkq/bluez-alsa
