========
a2dpconf
========

----------------------------------------
Decode A2DP codec capability hex strings
----------------------------------------

:Date: September 2024
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**a2dpconf** [*OPTION*]... [*CODEC*:]\ *CONFIG*...

DESCRIPTION
===========

**a2dpconf** presents the fields of the given A2DP codec *CONFIG* in a
human-readable format. *CODEC* is the name of the relevant codec, and *CONFIG*
is the hexadecimal encoding of the configuration or capabilities binary "blob"
as reported by tools such as ``bluealsactl(1)`` or the debug output of
``bluealsad(8)``.
(see `EXAMPLES`_ below).

OPTIONS
=======

-h, --help
    Print this help and exit.

-V, --version
    Print version and exit.

-x, --auto-detect
    Try to auto-detect the codec. If the name of the codec associated with the
    configuration string is not known, then give this option and the
    configuration string without the codec name prefix. The output is then a
    list of all possible known codec configurations for which the given string
    is valid.

EXAMPLES
========
::

    $ a2dpconf sbc:ffff0235
    SBC <hex:ffff0235> {
      sample-rate:4 = 48000 44100 32000 16000
      channel-mode:4 = JointStereo Stereo DualChannel Mono
      block-length:4 = 16 12 8 4
      sub-bands:2 = 8 4
      allocation-method:2 = Loudness SNR
      min-bit-pool-value:8 = 2
      max-bit-pool-value:8 = 53
    }

::

    $ a2dpconf -x ffff0235
    SBC <hex:ffff0235> {
      sample-rate:4 = 48000 44100 32000 16000
      channel-mode:4 = JointStereo Stereo DualChannel Mono
      block-length:4 = 16 12 8 4
      sub-bands:2 = 8 4
      allocation-method:2 = Loudness SNR
      min-bit-pool-value:8 = 2
      max-bit-pool-value:8 = 53
    }
    MPEG-1,2 Audio <hex:ffff0235> {
      layer:3 = MP3 MP2 MP1
      crc:1 = true
      channel-mode:4 = JointStereo Stereo DualChannel Mono
      <reserved>:1
      media-payload-format:1 = MPF-1 MPF-2
      sample-rate:6 = 48000 44100 32000 24000 22050 16000
      vbr:1 = false
      bitrate-index:15 = 0x235
    }

COPYRIGHT
=========

Copyright (c) 2016-2024 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``bluealsactl(1)``, ``bluealsad(8)``

Project web site
  https://github.com/arkq/bluez-alsa
