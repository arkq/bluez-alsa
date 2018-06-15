Bluetooth Audio ALSA Backend [![Build Status](https://travis-ci.org/Arkq/bluez-alsa.svg?branch=master)](https://travis-ci.org/Arkq/bluez-alsa)
============================

This project is a rebirth of a direct integration between [Bluez](http://www.bluez.org/) and
[ALSA](http://www.alsa-project.org/). Since Bluez >= 5, the build-in integration has been removed
in favor of 3rd party audio applications. From now on, Bluez acts as a middleware between an
audio application, which implements Bluetooth audio profile, and a Bluetooth audio device.

The current status quo is, that in order to stream audio from/to a Bluetooth device, one has to
install PulseAudio, or use Bluez < 5. However, Bluez version 4 is considered to be deprecated, so
the only reasonable way to achieve this goal is to install PulseAudio.

With this application (later named as BlueALSA), one can achieve the same goal as with PulseAudio,
but with less dependencies and more bare-metal-like. BlueALSA registers all known Bluetooth audio
profiles in Bluez, so in theory every Bluetooth device (with audio capabilities) can be connected.
In order to access the audio stream, one has to connect to the ALSA PCM device called `bluealsa`.
The device is based on the ALSA software PCM plugin.


Installation
------------

	$ autoreconf --install
	$ mkdir build && cd build
	$ ../configure --enable-aac --enable-debug

or if you intend to stream audio from a Linux distribution using PulseAudio (see [this
issue](https://github.com/Arkq/bluez-alsa/issues/13))

	$ ../configure --enable-aac --enable-debug --disable-payloadcheck

then

	$ make && make install

Dependencies:

- [alsa-lib](http://www.alsa-project.org/)
- [bluez](http://www.bluez.org/) >= 5.0
- [glib](https://wiki.gnome.org/Projects/GLib) with GIO support
- [sbc](https://git.kernel.org/cgit/bluetooth/sbc.git)
- [fdk-aac](https://github.com/mstorsjo/fdk-aac) (when AAC support is enabled with `--enable-aac`)
- [openaptx](https://github.com/Arkq/openaptx) (when apt-X support is enabled with `--enable-aptx`)

Dependencies for `hcitop` (when `--enable-hcitop` is specified during configuration):

- [libbsd](https://libbsd.freedesktop.org/)
- [ncurses](https://www.gnu.org/software/ncurses/)

If you are using Debian-based distribution, take a look at the [.travis.yml](.travis.yml) file,
it might give you a hint about required packages.


Configuration & Usage
---------------------

The main component of the BlueALSA is a program called `bluealsa`. It should be run as a root
during system startup (root privileges are not required per se, the only requirement is a write
access to `/var/run/bluealsa`). This program acts as a proxy between Bluez and ALSA.

In order to stream audio to the e.g. Bluetooth headset, firstly one has to connect the device. The
most straightforward method is to use Bluez CLI utility called `bluetoothctl`. When the device is
connected one can use the `bluealsa` virtual PCM device as follows:

	$ aplay -D bluealsa:HCI=hci0,DEV=XX:XX:XX:XX:XX:XX,PROFILE=a2dp Bourree_in_E_minor.wav

Setup parameters of the bluealsa PCM device can be set in the local `.asoundrc` configuration file
like this:

	$ cat ~/.asoundrc
	defaults.bluealsa.interface "hci0"
	defaults.bluealsa.device "XX:XX:XX:XX:XX:XX"
	defaults.bluealsa.profile "a2dp"
	defaults.bluealsa.delay 10000

BlueALSA also allows to capture audio from the connected Bluetooth device. To do so, one has to
use the capture PCM device, e.g.:

	$ arecord -D bluealsa capture.wav

Using this feature, it is possible to create Bluetooth-powered speaker. It is required to forward
audio signal from the BlueALSA capture PCM to some other playback PCM (e.g. build-id audio card).
In order to simplify this task, there is a program called `bluealsa-aplay`, which acts as a simple
BlueALSA player. Connect your Bluetooth device (e.g. smartphone) and do as follows:

	$ bluealsa-aplay XX:XX:XX:XX:XX:XX

In order to control input or output audio level, one can use provided `bluealsa` control plugin.
This plugin allows adjusting the volume of the audio stream or simply mute/unmute it, e.g.:

	$ amixer -D bluealsa sset '<control name>' 70%

where the control name is the name of a connected Bluetooth device with a control element suffix,
e.g.:

	$ amixer -D bluealsa sset 'Jabra MOVE v2.3.0 - A2DP' 50%

For more advanced ALSA configuration, consult the asoundrc on-line
[documentation](http://www.alsa-project.org/main/index.php/Asoundrc) provided by the AlsaProject
wiki page.


Troubleshooting
---------------

1. Using BlueALSA alongside with PulseAudio.

	Due to BlueZ limitations, it seems, that it is not possible to use BlueALSA and PulseAudio to
	handle Bluetooth audio together. BlueZ can not handle more than one application which registers
	audio profile in the Bluetooth stack. However, it is possible to run BlueALSA and PulseAudio
	alongside, but Bluetooth support has to be disabled in the PulseAudio. Any Bluetooth related
	module has to be unloaded - e.g. `bluetooth-discover`, `bluez5-discover`.

2. ALSA thread-safe API (alsa-lib >= 1.1.2).

	Starting from ALSA library 1.1.2, it is possible to enable thread-safe API functions. It is a
	noble change, but the implementation leaves a lot to be desired. This "minor" change does not
	affect hardware audio devices (because for hardware devices, this change is disabled), but it
	affects A LOT all software plug-ins. Random deadlocks are inevitable. My personal advice is to
	disable it during alsa-lib configuration step (`./configure --disable-thread-safety` - of
	course, if one is compiling alsa-lib from source), or if it is not possible (instalation from a
	package repository), disable it via an environmental variable, as follows: `export
	LIBASOUND_THREAD_SAFE=0`. Just take a look at involved
	[hacks](http://git.alsa-project.org/?p=alsa-lib.git;a=blob;f=src/pcm/pcm_ioplug.c;h=1dc198e7c99c933264fa25c9d7dbac5153bf0860;hb=1bf144013cffdeb41a5df3a11a8eb2596c5ea2b5#l682)
	(search for "to avoid deadlock" comments) and decide for yourself.


Resources
---------

1. [Bluetooth Adopted Specifications](https://www.bluetooth.com/specifications/adopted-specifications)
2. [Bluetooth Design Guidelines](https://developer.apple.com/hardwaredrivers/BluetoothDesignGuidelines.pdf)
3. [RTP Payload Format for MPEG-4](https://tools.ietf.org/html/rfc6416)
4. [Coding of MPEG-4 Audio](http://www.iso.org/iso/iso_catalogue/catalogue_tc/catalogue_detail.htm?csnumber=42739)
5. [ALSA project library reference](http://www.alsa-project.org/alsa-doc/alsa-lib/index.html)
