========
a2dpconf
========

----------------------------------------
Decode A2DP codec capability hex strings
----------------------------------------

:Date: December 2024
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

-v, --verbose
    Show verbose bit-stream details.
    Display each field as a binary mask with each bit represented by a single
    character.

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
      Sample Rate ( 16000 Hz | 32000 Hz | 44100 Hz | 48000 Hz )
      Channel Mode ( Mono | Dual Channel | Stereo | Joint Stereo )
      Block Length ( 4 | 8 | 12 | 16 )
      Sub-bands ( 4 | 8 )
      Allocation Method ( SNR | Loudness )
      Min Bit-pool ( 2 )
      Max Bit-pool ( 53 )
    }

::

    $ a2dpconf -x ffff0235
    SBC <hex:ffff0235> {
      Sample Rate ( 16000 Hz | 32000 Hz | 44100 Hz | 48000 Hz )
      Channel Mode ( Mono | Dual Channel | Stereo | Joint Stereo )
      Block Length ( 4 | 8 | 12 | 16 )
      Sub-bands ( 4 | 8 )
      Allocation Method ( SNR | Loudness )
      Min Bit-pool ( 2 )
      Max Bit-pool ( 53 )
    }
    MPEG-1,2 Audio <hex:ffff0235> {
      Layer ( MP1 | MP2 | MP3 )
      CRC ( true )
      Channel Mode ( Mono | Dual Channel | Stereo | Joint Stereo )
      RFA ( 1 )
      Media Payload Format ( MPF-1 | MPF-2 )
      Sample Rate ( 16000 Hz | 22050 Hz | 24000 Hz | 32000 Hz | ... )
      VBR ( false )
      Bitrate Index ( 0 | 2 | 4 | 5 | 9 )
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
