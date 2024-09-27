===========
bluealsactl
===========

----------------------------------------------------------
a simple command line interface for the BlueALSA D-Bus API
----------------------------------------------------------

:Date: September 2024
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**bluealsactl** [*OPTION*]... [*COMMAND* [*ARG*]...]

DESCRIPTION
===========

**bluealsactl** provides command-line access to the BlueALSA D-Bus API
"org.bluealsa.Manager1" and "org.bluealsa.PCM1" interfaces and thus allows
introspection and some control of BlueALSA PCMs while they are running.

OPTIONS
=======

-h, --help
    Output a usage message. When used before the *COMMAND* prints a list of
    options and commands. When used as a *COMMAND* *ARG* prints help specific
    to that *COMMAND*

-V, --version
    Output the version number.

-B NAME, --dbus=NAME
    BlueALSA service name suffix. For more information see ``--dbus``
    option of ``bluealsad(8)`` service daemon.

-q, --quiet
    Do not print any error messages.

-v, --verbose
    Include extra information in normal output - see COMMANDS_ for details.
    This option can be used multiple times to increase verbosity.

COMMANDS
========

If no *COMMAND* is given, the default is **status**.

All commands may be given the *ARG* **--help**, other *ARGs* are described
against each command below.

The *PCM_PATH* command argument, where required, must be a BlueALSA PCM D-Bus
path. Use the command **list-pcms** to obtain a list of valid PCM D-Bus paths.

status
    Print properties of the service: service name, build version, in-use
    Bluetooth adapters, available profiles and codecs. Example output:
    ::

        Service: org.bluealsa
        Version: v4.1.1
        Adapters: hci0 hci1
        Profiles:
          A2DP-source : SBC AAC
          HFP-AG      : CVSD mSBC
          HSP-AG      : CVSD

list-services
    Print a name list of all running BlueALSA D-Bus services, one per line.

list-pcms
    Print a list of BlueALSA PCM D-Bus paths, one per line.

    If the **--verbose** option is given then the properties of each connected
    PCM are printed after each path, one per line, in the same format as the
    **info** command.

info *PCM_PATH*
    Print the properties and available codecs of the given PCM.
    The properties are printed one per line, in the format
    "PropertyName: Value". Values are presented in human-readable format, and
    for a property with multiple values they are printed as a space-separated
    list.

    The Volume property is presented as two lines; "Volume:" indicating
    the loudness component of each channel and "Mute:" indicating the mute
    component of each channel. Both components are presented with their channel
    values in the same order as the ChannelMap. For example:
    ::

        ChannelMap: FC FL FR RL RR LFE
        Volume: 127 127 127 127 127 127
        Mute: off off off off off off

    If the **--verbose** option is given then "Available codecs:" values include
    the codec capabilities as a hexadecimal string suffix, separated by a colon.
    Similarly, the "Selected codec:" value includes the current codec
    configuration as a hexadecimal string separated by a colon. For example:
    ::

        Available codecs: SBC:ffff02fa AAC:c0ffff035b60
        Selected codec: AAC:400084035b60

    A tool such as ``a2dpconf(1)`` can be used to decode the hex string.

    The list of available A2DP codecs requires BlueZ SEP support
    (BlueZ >= 5.52)

codec [-c NUM] [-r NUM] [--force] *PCM_PATH* [*CODEC*\ [:*CONFIG*]]
    Get or set the Bluetooth codec used by the given PCM.

    If *CODEC* is not given, print a list of additional codecs supported by the
    given PCM and the currently selected codec. With the option **--verbose**
    the codec capabilities and current configuration are shown in the same
    format as for the **info** command.

    If *CODEC* is given, change the codec to be used by the given PCM. This
    command will terminate the PCM if it is currently running.

    Optionally, for A2DP codecs, one can specify A2DP codec configuration which
    should be selected. The *CONFIG* shall be given as a hexadecimal string. If
    this parameter is omitted, BlueALSA will select default configuration based
    on codec capabilities of connected Bluetooth device.

    Given A2DP codec configuration shall be a valid configuration in respect to
    the capabilities of connected Bluetooth device and the BlueALSA itself. If
    the given configuration is not valid, this command will fail with an error.
    In such case, one can use the **--force** option to force the selection of
    the configuration. However, this may result in a non-working connection and
    in the worst case it may crash remote Bluetooth device!

    The options **-c NUM** and **-r NUM** may be used to select a specific
    channel count and/or sample rate, provided that the given values are
    supported by the codec. For example, if the device at path PCM_PATH
    supports 5.1 surround sound and 96000 rate with the AAC codec, but is
    currently configured as AAC with stereo at 48000, then the configuration
    can be changed with:
    ::

        bluealsactl codec -c6 -r96000 PCM_PATH aac

    Selecting an A2DP codec and listing available A2DP codecs requires BlueZ
    SEP support (BlueZ >= 5.52).

    BlueALSA does not support changing the HFP codec from an HFP-HF node. The
    codec can only be changed from the HFP-AG node. Using the
    **bluealsactl codec** command to set the codec from an HFP-HF node fails,
    reporting an input/output error.

    Selecting the HFP codec when using oFono is not supported.

volume *PCM_PATH* [*VOLUME* [*VOLUME*]...]
    Get or set the volume loudness value(s) of the given PCM.

    If *VOLUME* is given, set the loudness component of the volume property of
    the given PCM.

    If only one value *VOLUME* is given it is applied to all channels.

    For multi-channel PCMs, if multiple *VOLUME* values are given, then each
    given value is applied to the corresponding channel of the ChannelMap (see
    the **info** command). If the number of values given is less than the
    number of channels, then the remaining channels are set to the first given
    value.

    Valid A2DP values for *VOLUME* are 0-127, valid HFP/HSP values are 0-15.

    Note that A2DP does not support independent channel volumes, so such a
    setting is better suited to use with soft-volume enabled. See
    ``bluealsad(8)`` for more details.

mute *PCM_PATH* [*STATE* [*STATE*]...]
    Get or set the mute switch of the given PCM.

    If *STATE* argument(s) are given, set mute component of the volume property
    of the given PCM. Multiple *STATE* arguments are used for multi-channel
    PCMs as described for the **volume** command.

    The *STATE* value can be one of **on**, **yes**, **true**, **y** or **1**
    for mute on, or **off**, **no**, **false**, **n** or **0** for mute off.

soft-volume *PCM_PATH* [*STATE*]
    Get or set the SoftVolume property of the given PCM.

    If the *STATE* argument is given, set the SoftVolume property for the given
    PCM. This property determines whether BlueALSA will make volume control
    internally or will delegate this task to BlueALSA PCM client or connected
    Bluetooth device respectively for PCM sink or PCM source.

    The *STATE* value can be one of **on**, **yes**, **true**, **y** or **1**
    for soft-volume on, or **off**, **no**, **false**, **n** or **0** for
    soft-volume off.

client-delay *PCM_PATH* [[-]\ *DELAY*]
    Get or set the ClientDelay property of the given PCM.

    If the *DELAY* argument is given, set the ClientDelay property for the
    given PCM. This property may be used by clients to
    adjust the reported audio delay and may be useful with PCM devices that do
    not report an accurate Delay property.

    The *DELAY* value is in milliseconds and must be a decimal number with
    optional sign prefix (e.g. **250**, **-500**, **+360.4**). The permitted
    range is [-3276.8, 3276.7].

monitor [-p[PROPS] | --properties[=PROPS]]
    Listen for D-Bus signals indicating adding/removing BlueALSA interfaces.
    Also detect service running and service stopped events, and optionally
    PCM property change events. Print a line on standard output for each one
    received.

    PCM event output lines are formed as:

    ``PCMAdded PCM_PATH``

    ``PCMRemoved PCM_PATH``

    If the **--verbose** option is given then the properties of each added PCM
    are printed after the PCMAdded line, one per line, in the same format as
    the **info** command. In this case a blank line is printed after the last
    property.

    RFCOMM event output lines are formed as:

    ``RFCOMMAdded RFCOMM_PATH``

    ``RFCOMMRemoved RFCOMM_PATH``

    Service start/stop event lines are formed as:

    ``ServiceRunning SERVICE_NAME``

    ``ServiceStopped SERVICE_NAME``

    When the monitor starts, it begins by printing a ``ServiceRunning`` or
    ``ServiceStopped`` message according to the current state of the service.

    If the **-p** or **--properties** option is given then also detect changes
    to certain PCM properties. Print a line on standard output for each
    property change. The output lines are formed as:

    ``PropertyChanged PCM_PATH PROPERTY_NAME VALUE``

    Property names that can be monitored are **Codec**, **Delay**,
    **ClientDelay**, **Running**, **SoftVolume** and **Volume**.

    Volume is an array of values, each showing the loudness and mute components
    of a channel. The order of the values corresponds to the ChannelMap
    property (see the **info** command). The loudness is shown as a decimal
    integer value, with an optional suffix ``[M]`` indicating that the channel
    is muted. For example, for a 2-channel (stereo) A2DP PCM at path PCM_PATH
    with both channels at full volume and the right channel muted, the event
    would be displayed as:
    ::

         PropertyChanged PCM_PATH Volume 127 127[M]

    *PROPS* is an optional comma-separated list of property names to be
    monitored. If given, only changes to those properties listed will be
    printed. If this argument is not given then changes to any of the above
    properties are printed.

open [--hex] *PCM_PATH*
    Transfer raw audio frames to or from the given PCM. For sink PCMs
    the frames are read from standard input and written to the PCM. For
    source PCMs the frames are read from the PCM and written to standard
    output. The format, channels and sample rate must match the properties
    of the PCM, as no format conversions are performed by this tool.

    With the **--hex** option, the data is read or written as hexadecimal
    strings.

COPYRIGHT
=========

Copyright (c) 2016-2024 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``a2dpconf(1)``, ``bluealsa-aplay(1)``, ``bluealsa-rfcomm(1)``,
``bluealsad(8)``

Project web site
  https://github.com/arkq/bluez-alsa
