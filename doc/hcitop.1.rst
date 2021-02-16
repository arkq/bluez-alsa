======
hcitop
======

-------------------------------------
a simple dynamic view of HCI activity
-------------------------------------

:Date: February 2021
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**hcitop** [*OPTION*]...

DESCRIPTION
===========

**hcitop** provides a dynamic real-time view of activity statistics for each
HCI interface. The view is refreshed at regular intervals, and also on demand
by pressing a key. To quit the program press the 'q' key, or use Ctrl-C.

OPTIONS
=======

-h, --help
    Output a usage message and exit.

-V, --version
    Output the version number and exit.

-d SEC, --delay=SEC
    Set the interval at which the statistics are refreshed. SEC is a number of
    seconds and may include a decimal point or exponent.

COLUMNS
=======

HCI
    The HCI name ("hci0", etc.).

FLAGS
    Status flags of the HCI. See ``FLAGS`` below.

RX
    Total amount of data received since the HCI was last brought up.

TX
    Total amount of data transmitted since the HCI was last brought up.

RX/s
    Average rate of reception during the last refresh interval.

TX/s
    Average rate of transmission during the last refresh interval.

FLAGS
=====

An array of flag characters indicating the current status of the HCI. The flags
are shown in the following order. The indicated letter appears when that flag is
"TRUE", a blank is shown when the flag is "FALSE".

U
    The interface is "Up".

I
    The interface is initializing.

R
    The interface is running.

P
    Page scan is enabled.

I
    Inquiry scan is enabled.

A
    Authentication is enabled.

E
    Encryption is enabled.

Q
    The interface is currently inquiring (scanning) remote devices.

X
    Raw mode is enabled.


SEE ALSO
========

``btmon(1)``, ``hciconfig(1)``, ``hcitool(1)``

**hcitop** is part of the **bluez-alsa** project.
Project web site at https://github.com/Arkq/bluez-alsa

COPYRIGHT
=========

Copyright (c) 2016-2021 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.
