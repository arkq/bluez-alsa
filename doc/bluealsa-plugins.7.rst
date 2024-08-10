================
bluealsa-plugins
================
----------------------------
Bluetooth Audio ALSA Plugins
----------------------------

:Date: June 2025
:Manual section: 7
:Manual group: Miscellaneous
:Version: $VERSION$

DESCRIPTION
===========

BlueALSA permits applications to access Bluetooth audio devices using the ALSA
alsa-lib API. Users of those applications can then use Bluetooth speakers,
headphones, headsets and hands-free devices much as if they were local devices.
This integration is achieved by two ALSA plugins, one for PCM audio streams and
one for CTL volume controls.

PCM PLUGIN
==========

The BlueALSA ALSA PCM plugin communicates with the ``bluealsad(8)`` service.
It can be used to define ALSA PCMs in your own configuration file (e.g.
~/.asoundrc), or you can use the predefined **bluealsa** PCM.

The Predefined **bluealsa** PCM
-------------------------------

The simplest way to use the PCM plugin is with the predefined ALSA PCM device
**bluealsa**. The definition of this PCM device is of type ``plug`` so audio
format conversion, if required, is done automatically by the PCM. It has
parameters DEV, PROFILE, CODEC, VOL, SOFTVOL, DELAY, and SRV. All these
parameters have defaults. Parameter values in an ALSA PCM name are specified
using the syntax:

::

  bluealsa:DEV=01:23:45:67:89:AB,PROFILE=a2dp,CODEC=aac,VOL=60,SOFTVOL=no,DELAY=0,SRV=org.bluealsa

PCM Parameters
~~~~~~~~~~~~~~

  DEV
    The device Bluetooth address in the form *XX:XX:XX:XX:XX:XX*. Device names
    or aliases are not valid here. The default value is **00:00:00:00:00:00**
    which selects the most recently connected device of the chosen profile.

  PROFILE
    May be either **a2dp** or **sco**. **sco** selects either Hands-Free (HFP)
    or Headset (HSP) profile, whichever is connected on the selected device.
    The default is **a2dp**.

  CODEC
    Specifies the codec to be used by the profile. When a connection is
    established between a device and a host, BlueALSA negotiates the best
    available codec with the device; this parameter allows the ALSA
    configuration to override that selection. The default value is
    **unchanged** which causes the PCM to use its existing codec setting. The
    codec name is case insensitive; so for example **aptX**, **aptx**, and
    **APTX** are all accepted. If the specified codec is not available the
    plugin issues a warning and uses the default value instead.

    BlueALSA does not support changing the HFP codec from a HFP-HF node, only
    the HFP-AG node can change the HFP codec.

    oFono does not permit the audio agent to select the codec, so this
    parameter has no effect when BlueALSA is used with oFono for HFP support.

    For the A2DP profile it is possible to also specify a "configuration" for
    the codec by appending the configuration as a hex string separated from the
    codec name by a colon. The bits responsible for the number of channels and
    the sample rate are set by the plugin with the respect to options
    provided by the user (channel mode and sample rate bits act as a
    mask). For example:

    ::

      CODEC=SBC:FC450240

    This SBC configuration limits the channel mode options to mono and dual
    channel. So, in case of 2 channel audio stream, the plugin will negotiate
    the dual channel mode instead of default (if supported) joint stereo mode.

  VOL
    Specifies the initial volume for the PCM when opened. The default value is
    **unchanged** which causes the PCM to use its existing volume setting. The
    value is an integer percentage of the maximum volume [0-100]. The mute
    status can also be set by appending the character '-' to mute the sound or
    '+' to unmute it. The volume is not restored to its original value when the
    PCM is closed. For example to set the initial volume to 80% and ensure that
    mute is disabled for this PCM:

    ::

      VOL=80+

  SOFTVOL
    Enables or disables BlueALSA's software volume feature for this PCM. See
    the ``bluealsad(8)`` manual page for more information on software volume.
    This is a boolean option (values **on** or **off**), but also accepts the
    special value **unchanged** which causes the PCM to use its existing
    softvol value. The default value is **unchanged**.

  HWCOMPAT
    Modifies the behavior of the plugin when the remote device is connected
    but not active in order to align better with the behavior of the ALSA
    ``hw`` plugin . This is a string option which takes the values **none**,
    **busy** or **silence**.
    See `Transport acquisition`_ in the **NOTES** section below for more
    information.

  DELAY
    An integer number which is added to the reported delay (latency) value in
    order to manually adjust the audio synchronization. It is not normally
    required and defaults to **0**. See the **EXT** parameter of the CTL plugin
    in the `CTL Parameters`_ section below for a more flexible and convenient
    method of manually adjusting the reported delay by using a mixer control.

  SRV
    The D-Bus service name of the BlueALSA daemon. Defaults to
    **org.bluealsa**. See ``bluealsad(8)`` for more information. Not normally
    required.

Setting Different Defaults
~~~~~~~~~~~~~~~~~~~~~~~~~~

The defaults can be overridden by defining the ones you want to change in your
own configuration (e.g. in ~/.asoundrc.conf) for example:

::

  defaults.bluealsa.device "00:11:22:33:44:55"
  defaults.bluealsa.profile "sco"
  defaults.bluealsa.codec "cvsd"
  defaults.bluealsa.volume "50+"
  defaults.bluealsa.softvol off
  defaults.bluealsa.hwcompat "silence"
  defaults.bluealsa.delay 5000
  defaults.bluealsa.service "org.bluealsa.source"

Note that **volume** takes a string value and so the default must be enclosed
in quotation marks.

Positional Parameters
~~~~~~~~~~~~~~~~~~~~~

ALSA permits arguments to be given as positional parameters as an alternative
to explicitly naming them. When using positional parameters it is important
that the values are given in the correct sequence - *DEV*, *PROFILE*, *CODEC*,
*VOL*, *SOFTVOL*, *HWCOMPAT*, *DELAY*, *SRV*. For example:

::

  bluealsa:01:23:45:67:89:AB,a2dp,unchanged,unchanged,unchanged,none,0,org.bluealsa

When using positional parameters defaults can only be implied at the end of the
id string, so

::

  bluealsa:01:23:45:67:89:AB

is equivalent to the full form above, but

::

    bluealsa:01:23:45:67:89:AB,a2dp,,80+

is not permitted.

Defining BlueALSA PCMs
----------------------

You can define your own ALSA PCM in the ALSA configuration. To do this, create
an ALSA configuration node defining a PCM with type ``bluealsa``. The
configuration node has the following fields:

::

  pcm.name {
    type bluealsa     # Bluetooth PCM
    device STR        # Device address in format XX:XX:XX:XX:XX:XX
    profile STR       # Profile type (a2dp or sco)
    [codec STR]       # Preferred codec
    [volume STR]      # Initial volume for this PCM
    [softvol BOOLEAN] # Enable/disable BlueALSA's software volume
    [hwcompat STR]    # HW compatibility mode (none, busy or silence)
    [delay INT]       # Extra delay (frames) to be reported (default 0)
    [service STR]     # DBus name of service (default org.bluealsa)
  }

The **device** and **profile** fields must be specified so that the plugin can
select the correct Bluetooth transport; the other fields are optional. Note
that the default values for the optional fields are not overridden
automatically by the configuration ``defaults.bluealsa.*`` in a PCM defined
this way; however the configuration defaults can be referenced by use of
``@func refer`` (see the `ALSA configuration file syntax` documentation for
more information).

When choosing a name for your PCM definition, the name **pcm.bluealsa** is
predefined by the bluez-alsa installation (see section *The Predefined
bluealsa PCM* above), so it should not be used as a name for your own PCM
devices as doing so will most likely have unexpected or undesirable results.

Note that the **volume** field is of type **string**, so the value must be
enclosed in double-quotes. See the *PCM Parameters* section above for more
information on each field.

Do not confuse the PCM type **bluealsa** with the PCM named **bluealsa**. The
type does not perform any audio conversions, you will have to wrap your own
defined PCMs with type **plug** to achieve that; whereas the predefined PCM
**pcm.bluealsa** *is* of type **plug**.

Name Hints
----------

Applications that follow ALSA guidelines will obtain the list of defined PCMs
by using the alsa-lib ``namehints`` API. To make BlueALSA PCMs visible via that
API it is necessary to add a "hint" section to the ALSA configuration. If you
have defined a new PCM, then the hint goes into the PCM configuration entry as
follows:

::

  pcm.bt-headphones {
      type plug
      slave.pcm {
          type bluealsa
          device "00:11:22:33:44:55"
          profile "a2dp"
      }
      hint {
          show on
          description "My Bluetooth headphones"
      }
  }

Now using ``aplay -L`` will include the following in its output:

::

  # aplay -L
  bt-headphones
      My Bluetooth headphones
  #

If you are using the predefined **bluealsa** PCM, then you can create a
"namehint" entry in your ~/.asoundrc file like this:

::

  namehint.pcm {
      mybluealsadevice "bluealsa:DEV=00:11:22:33:44:55,PROFILE=a2dp|My Bluetooth headphones"
  }

Then ``aplay -L`` shows

::

  # aplay -L
  bluealsa:DEV=00:11:22:33:44:55,PROFILE=a2dp
      My Bluetooth headphones

For alsa-lib versions before v1.2.3.2, a bug in the namehint parser means that
a **namehint.pcm** entry has to be written as

::

  namehint.pcm {
      mybluealsadevice "bluealsa:DEV=00:11:22:33:44:55,PROFILE=a2dp|DESCMy Bluetooth headphones"
  }

(note the keyword **DESC** after the pipe symbol and before the description
text.)

With that hint in place, the PCM will be listed as both a Capture and Playback
device. So ``arecord -L`` will also list it. That is generally OK for HFP/HSP
devices, but an A2DP device most often offers only Capture (e.g. a mobile
phone) or only Playback (e.g. a Bluetooth speaker). It is possible to use the
hint description to limit the listing to only one direction using an
undocumented syntax of ALSA configuration files.

If the hint.description value ends with **|IOIDInput** the PCM will only show
in listings of Capture devices; if it ends with **|IOIDOutput** the PCM will
only show in listings of Playback devices.

So we can modify our example above to:

::

  pcm.bt-headphones {
      type plug
      slave.pcm {
          type bluealsa
          device "00:11:22:33:44:55"
          profile "a2dp"
      }
      hint {
          show on
          description "My Bluetooth headphones|IOIDOutput"
      }
  }

or

::

  namehint.pcm {
      mybluealsadevice "bluealsa:DEV=00:11:22:33:44:55,PROFILE=a2dp|My Bluetooth headphones|IOIDOutput"
  }

Now the ``aplay -L`` output will be exactly the same as before, but ``arecord
-L`` will not include bt-headphones in its output.

When using the **namehint.pcm** method, the key (**mybluealsadevice** in the
above example) must be unique but otherwise is not used. The first part of the
value string, before the pipe | symbol, is the string that is to be passed to
ALSA applications to identify the PCM (e.g. with ``aplay -D ...``). The next
section, after the pipe symbol, is the description that will be presented to
the user. The optional **|IOID** section is not included in the description
given to the application.

CTL PLUGIN
==========

The BlueALSA ALSA CTL plugin can be used to define ALSA CTLs (mixer devices) in
your own configuration file (e.g. ~/.asoundrc), or you can use the predefined
configuration that is included in the bluez-alsa project.

A BlueALSA CTL device has no associated soundcard, so ``alsamixer`` will not
list it in its F6 menu. It can be selected either by starting ``alsamixer``
with

::

  alsamixer -D bluealsa

or by selecting "enter device name .." on the F6 menu then typing out
"bluealsa" in the "Device Name" box.


The CTL has two operating modes, **Default** mode and **Single Device** mode.

Default Mode
------------

In this mode when a device connects, the mixer will create new controls for it,
and when a device disconnects, the mixer will remove its controls.
``alsamixer(1)`` will show these changes dynamically.

Control names are constructed by combining the device Bluetooth alias with
either the profile type ('A2DP' or 'SCO') of the controlled PCM or the word
"Battery" for battery level indicators. If two or more connected devices have
the same alias then an index number is added to the name to make it unique.

The Bluetooth "alias" of a device is by default the same as its "name". The
name is a string defined by the device manufacturer and embedded in its
firmware. Typically two identical devices will have identical names. The
"alias" is created by BlueZ and stored locally on the host computer. So the
alias can be changed using a tool such as ``bluetoothctl(1)`` to make it unique
if desired. As manufacturers tend to use long names for their devices the alias
can also be useful to give a short "nickname" to a device.

Although this default mode works well with ``alsamixer``, there are some
limitations that may make it unsuitable for some applications. In particular:

- If device aliases are not unique then the index number associated with
  each is not easily predictable in advance; so it can be difficult to
  programmatically associate a PCM with its volume control.

- A consequence of the alsa-lib implementation of controls is that when one
  Bluetooth device connects or disconnects it is necessary to remove all
  controls from all devices in the mixer and create a new set. This invalidates
  pointers held by applications and can cause application crashes. (Hardware
  sound cards do not have randomly appearing and disappearing controls, so
  many, or even most, applications are not programmed correctly to deal with
  it.)

Single Device Mode
------------------

The BlueALSA CTL also implements an alternative mode that presents controls
only for one specified device. In this case the control names are simply the
profile type of the controlled PCM ('A2DP' or 'SCO') or the word "Battery".
There is never any need for index suffixes or device alias. Immediately this
overcomes the two main issues of the default mode.

Single device mode is achieved by including the device Bluetooth address as an
argument to the ALSA device id, for example:

::

  alsamixer -D bluealsa:00:11:22:33:44:55

A notable difference between single-device mode and the default mode is in the
cases of the device not being connected when the mixer is opened, and when the
device disconnects while the mixer is open.

For the default mode, the mixer will still open, even if no devices are
connected, but will display no controls. In single device mode the open request
will fail with an error message.

Similarly, in default mode when a device disconnects the mixer remains open but
removes the set of controls and creates a new control set without the
disconnected device. That new set will be empty if no devices remain. If the
device then re-connects the mixer will again create a new set of controls with
the newly connected device included.

In single device mode when its device disconnects then the mixer will close.
The ``alsamixer`` application will continue running with no associated device
or controls, but will not automatically re-open the mixer if the device
re-connects. The user can use F6 to open a new device.

As a special case, a single device mixer can be opened with the address
**00:00:00:00:00:00**. This will create a mixer with controls for the most
recently connected device at the time the mixer is opened. Once created, that
mixer behaves the same as if it had been opened with the actual address of the
device: it does not change to a new device if another is subsequently
connected.

The Predefined **bluealsa** CTL
-------------------------------

The **bluealsa** CTL has parameters DEV, EXT, BTT, DYN, and SRV. All the
parameters have defaults.

CTL Parameters
~~~~~~~~~~~~~~

  DEV
    The device Bluetooth address in the form XX:XX:XX:XX:XX:XX. Device names or
    aliases are not valid here. The default value is **FF:FF:FF:FF:FF:FF**
    which selects controls from all connected devices (see `Default Mode`_
    above). Also accepts the special address **00:00:00:00:00:00** which
    selects the most recently connected device.

  EXT
    Causes the plugin to include extra controls. These are the controls for
    Bluetooth codec selection, volume mode selection, client delay (sync)
    and/or battery level indicator.
    If the value is **yes** then all of these additional controls are included;
    if the value is **no** then none of them are included. The default is
    **no**.

    This parameter can also select individual controls by using a colon (':')
    separated list of control names. The control names are **codec**, **mode**,
    **sync** and **battery**. For example:

    ::

        EXT=codec
        EXT=mode:battery

    See `Codec switching`_ in the **NOTES** section below for more information
    on the codec selection control.

    The volume mode controls take values "software" and "pass-through"; the
    playback control has index 0 and capture control has index 1.
    See the `Volume control` section in the ``bluealsad(8)`` for more
    information on the software volume setting.

    The client delay controls are called "Sync". They can be used to apply
    a fixed adjustment to the delay reported by the associated PCM to the
    application, and may be useful with applications that need to synchronize
    the bluetooth audio stream with some some other stream, such as a video.
    The values are in milliseconds from ``-3275 ms`` to ``+3275 ms`` in steps
    of ``25 ms``. The playback control has index 0 and the capture control has
    index 1. Each codec supported by a PCM has its own client delay value.
    Note that this control changes only the delay value reported to the
    application by ALSA, it does not affect the actual delay (latency) of the
    PCM stream. Values set by this control type are saved in the BlueALSA
    persistent state files, and so are remembered and automatically applied
    each time the PCM is used.

    The read-only battery level indicator will be shown only if the device
    supports battery level reporting.

  BTT
    Appends Bluetooth transport type (e.g. "-SNK" or "-HFP-AG") to the control
    element names. When using with the `Default Mode`_ this will reduce the
    number of available characters for Bluetooth device name, so the default
    value is **no**.

    In some rare circumstances, when more than one A2DP or HFP/HSP profile is
    connected with a single Bluetooth device, it might happen that the control
    element names for such device will not be unique. This might be problematic
    for control applications which use ALSA High Level Control Interface, e.g.
    ``amixer`` or ``alsamixer``. Such applications will report error or simply
    crash. This can be avoided by setting the BTT parameter to **yes**.

  DYN
    Enables "dynamic" operation. The plugin will add and remove controls as
    profiles are connected or disconnected. This is the normal behavior, so
    the default value is "**yes**". This argument is ignored in default mode;
    in that mode operation is always dynamic. There are some applications that
    are not programmed to handle dynamic addition or removal of controls, and
    can fail when such events occur. Setting this argument to **no** in single
    device mode with such applications can protect them from such failures.
    When dynamic operation is disabled, the plugin never adds or removes any
    controls. If a single profile is disconnected, then its associated volume
    control is put into an inactive state, i.e.: read-only with its value and
    playback/capture switch set to 0.

  SRV
    The D-Bus service name of the BlueALSA daemon. Defaults to
    **org.bluealsa**. See ``bluealsad(8)`` for more information.

The default values can be overridden in the ALSA configuration, for example:

::

  defaults.bluealsa.ctl.device "00:11:22:33:44:55"
  defaults.bluealsa.ctl.bttransport "no"
  defaults.bluealsa.ctl.dynamic "yes"
  defaults.bluealsa.ctl.extended "no"

Defining BlueALSA CTLs
----------------------

You can define your own ALSA CTL in the ALSA configuration. To do this, create
an ALSA configuration node defining a CTL with type ``bluealsa``. The
configuration node has the following fields:

::

  ctl.name {
    type bluealsa     # Bluetooth PCM
    [device STR]      # Device address (default "FF:FF:FF:FF:FF:FF")
    [extended STR]    # Include additional controls (default no)
    [bttransport STR] # Append BT transport to element names (yes/no, default no)
    [dynamic STR]     # Enable dynamic operation (yes/no, default yes)
    [service STR]     # D-Bus name of service (default "org.bluealsa")
  }

All the fields (except **type**) are optional. See the `CTL Parameters`_
section above for more information on each field. As for PCM definitions above,
the default values for the optional fields are hard-coded into the plugin; they
are not overridden by the configuration ``defaults.bluealsa.`` settings.

NOTES
=====

Codec selection
---------------

When used on a HFP gateway node, there may be a brief delay with HFP PCMs
after connection until the codec is selected. This delay is typically less
than two seconds. During this time interval it is not possible to open the
PCM plugin, it will fail with "Resource temporarily unavailable" (EAGAIN).

Codec switching
---------------

Changing the codec used by a BlueALSA transport causes the PCM(s) running on
that transport to terminate. Therefore using a Codec control can have
undesirable consequences. Unfortunately the ``alsamixer(1)`` UI does not
present a separate pick-list for enumerated types, so merely browsing the list
of codecs using this control actually issues a Codec change request every time
a different codec is displayed. This is not ideal, so the use of this control
type with ``alsamixer(1)`` is not recommended. The control type does however
work well with other mixer applications such as ``amixer(1)``.

Note that BlueALSA does not support changing the HFP codec from a HFP-HF node,
only the HFP-AG node can change the HFP codec.

Transport acquisition
---------------------

The audio connection of a Bluetooth profile is not established immediately that
a device connects. The A2DP source device, or HFP/HSP gateway device, must
first "acquire" the profile transport.

When the BlueALSA PCM plugin is used on a source A2DP or gateway HFP/HSP node,
then **bluealsad(8)** will automatically acquire the transport and begin audio
transfer when the plugin starts the PCM.

When used on an A2DP sink or HFP/HSP HF/HS node then **bluealsad(8)** must wait
for the remote device to acquire the transport. The ALSA PCM plugin state model
does not define any state that can be directly mapped to this situation, so
the BlueALSA PCM plugin offers a choice of behaviors to suit various
application requirements. The choice is selected using the parameter
**hwcompat** (**HWCOMPAT** argument to the pre-defined `bluealsa` PCM) which
takes one of the following values:

- none

    The streams are presented exactly as handled by Bluetooth. No adjustments
    are made to align the PCM more to expected ALSA behavior. While waiting for
    the transport to be acquired the PCM plugin behaves as if the device
    timer is stopped; it does not generate any poll() events, and the
    application will be blocked when writing or reading to/from the PCM. For
    applications playing audio from a file or recording audio to a file this is
    not normally an issue and has the advantage that the played or captured
    stream does not contain any frames of silence artificially inserted by the
    plugin. However when streaming between some other device and a
    BlueALSA device this may lead to very large latency (delay) or trigger
    underruns or overruns in the other device. Capture streams may also have
    brief interruptions caused by Bluetooth radio link interference. Some
    applications, particularly ones which attempt to manage latency such as
    ``alsaloop(1)``, may become unstable in this situation.

- busy

    Causes snd_pcm_open() to return immediately with error code **-EBUSY**
    ("Device or resource busy") on A2DP sink, HFP-HF and HSP-HS nodes if the
    transport is not yet acquired. This is analogous to a ``hw`` device PCM
    that is temporarily unavailable (for example because it is in use by some
    other application). With this option the plugin also stops the
    PCM stream and enters the **SND_PCM_STATE_DISCONNECTED** state if the
    remote device releases the transport while in use, which is analogous to a
    removable ``hw`` device being unplugged. If a capture stream is interrupted
    by temporary Bluetooth link instability then the plugin simply blocks
    temporarily, which may cause issues for some applications as noted for the
    **none** value above.

- silence

    Inserts silence for capture streams, or simply drops frames for playback
    streams, whenever the transport is not acquired. Short intervals of silence
    may also be inserted into capture streams if there is a break in the
    incoming stream (for example as a result Bluetooth link instability). By
    this means a continuous stream is maintained as far as the application is
    concerned. This is analogous to a soundcard device with no speakers or
    microphone plugged in: only silence is captured and playback succeeds but
    produces no sound.

The **silence** option can also be used with capture devices on HFP/HSP AG and
A2DP source nodes (e.g. when using the FastStream codec) because in those cases
it is possible that the remote device is not sending any audio even though the
transport has been acquired.

PCM drain and non-blocking operation
------------------------------------

The BlueALSA PCM plugin does not support draining of capture PCMs. For a
capture PCM `snd_pcm_drain()` has the same effect as `snd_pcm_drop()`. This is
a limitation of the ALSA `ioplug` external plugin API.

For playback PCMs, BlueALSA has support for the drain operation in both
blocking and non-blocking modes. In blocking mode the drain operation will wait
until the BlueALSA server has played out the final audio frame. In non-blocking
mode the plugin will inform the application of drain completion as soon as the
ALSA ring buffer has been flushed; this means that some audio frames at the end
of the stream may be lost in non-blocking mode as the PCM may stop before the
server has had time to encode and play out all the frames.

FILES
=====

/etc/alsa/conf.d/20-bluealsa.conf
    BlueALSA device configuration file.
    ALSA additional configuration, defines the ``bluealsa`` PCM and CTL
    devices.

COPYRIGHT
=========

Copyright (c) 2016-2025 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.

SEE ALSO
========

``alsamixer(1)``, ``amixer(1)``, ``aplay(1)``, ``bluetoothctl(1)``,
``bluealsad(8)``, ``bluetoothd(8)``

Project web site
  https://github.com/arkq/bluez-alsa

ALSA configuration file syntax
  https://www.alsa-project.org/alsa-doc/alsa-lib/conf.html

ALSA built-in PCM plugins reference
  https://www.alsa-project.org/alsa-doc/alsa-lib/pcm_plugins.html
