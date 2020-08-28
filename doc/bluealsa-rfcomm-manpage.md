% BLUEALSA-RFCOMM(1) $VERSION$ | General Commands Manual
%
% August 2020

# NAME

bluealsa-rfcomm - a simple RFCOMM terminal for bluealsa

# SYNOPSIS

**bluealsa-rfcomm** [*OPTION*]... *DEVICE-PATH*

# DESCRIPTION

Bluealsa-rfcomm provides access to HSP/HFP RFCOMM terminal for connected Bluetooth device
specified by the *DEVICE-PATH*. The *DEVICE-PATH* can be either a Bluetooth device D-Bus
path defined by BlueZ (org.bluez) or BlueALSA (org.bluealsa) service.

# OPTIONS

**-h**, **\--help**
:   Output a usage message and exit.

**-V**, **\--version**
:   Output the version number and exit.

**-B**, **\--dbus**=*NAME*
:   BlueALSA service name suffix. For more information see **\--dbus**=*NAME*
    option of **bluealsa**(8) service daemon.

# EXAMPLE

    bluealsa-rfcomm /org/bluealsa/hci0/dev_1C_48_F9_9D_81_5C
    1C:48:F9:9D:81:5C> RING
    > AT+IPHONEACCEV=2,1,6,2,0
    > AT+CKPD=200
    disconnected

# SEE ALSO

**bluealsa**(8), **bluealsa-aplay**(1)

Project web site at https://github.com/Arkq/bluez-alsa

# COPYRIGHT

Copyright (c) 2016-2020 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.
