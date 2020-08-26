% BLUEALSA-APLAY(1)
% Arkadiusz Bokowy
% August 2020

# NAME

bluealsa-aplay - a simple bluealsa player

# SYNOPSIS

bluealsa-aplay [OPTION]... [BT-ADDR]...

# DESCRIPTION

Capture audio streams from bluetooth devices (via bluealsa(1)) and play them to an ALSA playback device.

# OPTIONS

**-h**, **--help**  
print this help and exit

**-V**, **--version**  
print version and exit

**-v**, **--verbose**  
make output more verbose

**-l**, **--list-devices**  
list available BT audio devices

**-L**, **--list-pcms**  
list available BT audio PCMs

**-B**, **--dbus**=_NAME_  
BlueALSA service name suffix

**-D**, **--pcm**=_NAME_  
playback PCM device to use

**--pcm-buffer-time**=_INT_  
playback PCM buffer time

**--pcm-period-time**=_INT_  
playback PCM period time

**--profile-a2dp**  
use A2DP profile (default)

**--profile-sco**  
use SCO profile

**--single-audio**  
single audio mode

### Note

If one wants to receive audio from more than one Bluetooth device, it is
possible to specify more than one MAC address. By specifying any/empty MAC
address (00:00:00:00:00:00), one will allow connections from any Bluetooth
device. Without given explicit MAC address any/empty MAC is assumed.

# COPYRIGHT

Copyright (c) 2016-2020 Arkadiusz Bokowy.
bluealsa-aplay is part of the bluez-alsa project
https://github.com/Arkq/bluez-alsa
and is licensed under the terms of the MIT license.

# SEE ALSO

bluealsa(1)

