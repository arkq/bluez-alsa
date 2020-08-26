% BLUEALSA(1)
% Arkadiusz Bokowy
% August 2020

# NAME

bluealsa - Bluetooth Audio ALSA Backend

# SYNOPSIS

**bluealsa [OPTION]...**

# DESCRIPTION

bluealsa is a Linux daemon to give applications access to bluetooth audio streams using the bluetooth A2DP, HSP, and HFP profiles. It provides a D-Bus API to applications, and can be used by ALSA applications via libasound plugins.

# OPTIONS

**-h**, **--help**  
print this help and exit

**-V**, **--version**  
print version and exit

**-B**, **--dbus**=_NAME_  
D-Bus service name suffix

**-S**, **--syslog**  
send output to syslog

**-i**, **--device**=_hciX_  
HCI device(s) to use

**-p**, **--profile**=_NAME_  
enable BT profile. Available bluetooth profiles are:

  - a2dp-source Advanced Audio Source (SBC)
  - a2dp-sink   Advanced Audio Sink (SBC)
  - hfp-hf      Hands-Free (v1.7)
  - hfp-ag      Hands-Free Audio Gateway (v1.7)
  - hsp-hs      Headset (v1.2)
  - hsp-ag      Headset Audio Gateway (v1.2)

By default only output profiles are enabled, which includes A2DP Source and
HSP/HFP Audio Gateways. If one wants to enable other set of profiles, it is
required to explicitly specify all of them using \`-p NAME\` options.

**--a2dp-force-mono**  
force monophonic sound

**--a2dp-force-audio-cd**  
force 44.1 kHz sampling

**--a2dp-keep-alive**=_SEC_  
keep A2DP transport alive

**--a2dp-volume**  
native volume control by default

**--sbc-quality**=_NB_  
set SBC encoder quality, where `NB` can be one of:

  - `0` - low audio quality (mono: 114 kbps, stereo: 213 kbps)
  - `1` - medium audio quality (mono: 132 kbps, stereo: 237 kbps)
  - `2` - high audio quality (mono: 198 kbps, stereo: 345 kbps) (**default**)
  - `3` - SBC Dual Channel HD (SBC XQ) (452 kbps)

**--mp3-quality**=_NB_  
selects LAME encoder internal algorithm. True quality is determined by the bitrate but this option will effect quality by selecting expensive or cheap algorithm. The `NB` can be in the range 0 - 9:

  - `0` - best quality, but requires a lot of CPU power
  - ...
  - `5` - good quality, fast (**default**)
  - ...
  - `9` - worst quality, very fast

**--mp3-vbr-quality**=_NB_  
specifies variable bit rate (VBR) quality, where `NB` can be in the range 0 - 9:

  - `0` - highest quality VBR mode
  - `1` - higher quality VBR mode
  - `2` - high quality VBR mode (**default**)
  - ...
  - `9` - lowest quality VBR mode

**--aac-afterburner**  
enables Fraunhofer AAC afterburner feature, which is a type of analysis by synthesis algorithm. This feature increases the audio quality by the cost of increased processing power and overall memory consumption.

**--aac-latm-version**=_NB_  
allows to specify LATM syntax version used for AAC audio transport. The `NB` can be one of:

  - `0` - LATM syntax specified by ISO-IEC 14496-3 (2001), should work with all older BT devices
  - `1` - LATM syntax specified by ISO-IEC 14496-3 (2005), should work with newer BT devices (**default**)

**--aac-vbr-mode**=_NB_  
specifies encoder variable bit rate (VBR) quality, or disables it. The `NB` can be one of:

  - `0` - disables variable bit rate mode and uses constant bit rate specified by the A2DP AAC configuration
  - `1` - lowest quality VBR mode (mono: 32 kbps, stereo: 40 kbps)
  - `2` - low quality VBR mode (mono: 40 kbps, stereo: 64 kbps)
  - `3` - medium quality VBR mode (mono: 56 kbps, stereo: 96 kbps)
  - `4` - high quality VBR mode (mono: 72 kbps, stereo: 128 kbps) (**default**)
  - `5` - highest quality VBR mode (mono: 112 kbps, 192 kbps)

**--ldac-abr**  
enables adaptive bit rate, which will dynamically adjust encoder quality based on the connection stability.

**--ldac-eqmid**=_NB_  
specifies encoder quality, where `NB` can be one of:

  - `0` - high quality (44.1 kHz: 909 kbps, 48 kHz: 990 kbps)
  - `1` - standard quality (44.1 kHz: 606 kbps, 48 kHz: 660 kbps) (**default**)
  - `2` - mobile quality (44.1 kHz: 303 kbps, 48 kHz: 330 kbps)


# COPYRIGHT

Copyright (c) 2016-2020 Arkadiusz Bokowy.
bluealsa is part of the bluez-alsa project
https://github.com/Arkq/bluez-alsa
and is licensed under the terms of the MIT license.

# SEE ALSO

bluealsa-aplay(1)

