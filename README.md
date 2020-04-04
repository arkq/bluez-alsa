Bluetooth Audio ALSA Backend [![Build Status](https://travis-ci.org/Arkq/bluez-alsa.svg?branch=master)](https://travis-ci.org/Arkq/bluez-alsa)
============================

This project is a rebirth of a direct integration between [BlueZ](http://www.bluez.org/) and
[ALSA](https://www.alsa-project.org/). Since BlueZ >= 5, the build-in integration has been removed
in favor of 3rd party audio applications. From now on, BlueZ acts as a middleware between an
audio application, which implements Bluetooth audio profile, and a Bluetooth audio device.

The current status quo is, that in order to stream audio from/to a Bluetooth device, one has to
install [PulseAudio](https://www.freedesktop.org/wiki/Software/PulseAudio), or use BlueZ < 5.
However, BlueZ version 4 is considered to be deprecated, so the only reasonable way to achieve
this goal is to install PulseAudio.

With this application (later named as BlueALSA), one can achieve the same goal as with PulseAudio,
but with less dependencies and more bare-metal-like. BlueALSA registers all known Bluetooth audio
profiles in BlueZ, so in theory every Bluetooth device (with audio capabilities) can be connected.
In order to access the audio stream, one has to connect to the ALSA PCM device called `bluealsa`.
Please note that this PCM device is based on the [ALSA software PCM I/O
plugin](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm_external_plugins.html) - it will not be
available in the [ALSA Kernel proc
interface](https://www.kernel.org/doc/html/latest/sound/designs/procfile.html).


Installation
------------

	$ autoreconf --install
	$ mkdir build && cd build
	$ ../configure --enable-aac --enable-ofono --enable-debug

or if you intend to stream audio from a Linux distribution using PulseAudio < 13.0 (see [this
issue](https://github.com/Arkq/bluez-alsa/issues/13))

	$ ../configure --enable-aac --enable-ofono --enable-debug --disable-payloadcheck

then

	$ make && make install

Dependencies:

- [alsa-lib](https://www.alsa-project.org/)
- [bluez](http://www.bluez.org/) >= 5.0
- [glib](https://wiki.gnome.org/Projects/GLib) with GIO support
- [sbc](https://git.kernel.org/cgit/bluetooth/sbc.git)
- [mp3lame](https://lame.sourceforge.net/) (when MP3 support is enabled with `--enable-mp3lame`)
- [mpg123](https://www.mpg123.org/) (when MPEG decoding support is enabled with `--enable-mpg123`)
- [fdk-aac](https://github.com/mstorsjo/fdk-aac) (when AAC support is enabled with `--enable-aac`)
- [openaptx](https://github.com/Arkq/openaptx) (when apt-X encoding support is enabled with
	`--enable-aptx` and/or `--enable-aptx-hd`)
- [libldac](https://github.com/EHfive/ldacBT) (when LDAC encoding support is enabled with
	`--enable-ldac`)

Dependencies for client applications (e.g. `bluealsa-aplay`):

- [libdbus](https://www.freedesktop.org/wiki/Software/dbus/)

Dependencies for `bluealsa-rfcomm` (when `--enable-rfcomm` is specified during configuration):

- [readline](https://tiswww.case.edu/php/chet/readline/rltop.html)

Dependencies for `hcitop` (when `--enable-hcitop` is specified during configuration):

- [libbsd](https://libbsd.freedesktop.org/)
- [ncurses](https://www.gnu.org/software/ncurses/)

For a comprehensive installation guide, please look at the [Installation from
source](https://github.com/Arkq/bluez-alsa/wiki/Installation-from-source) bluez-alsa wiki page. If
you've found something missing or incorrect, fill free to make a wiki contribution. Alternatively,
if you are using Debian-based distribution, take a look at the [.travis.yml](.travis.yml) file, it
might give you a hint about required packages.


Configuration & Usage
---------------------

The main component of BlueALSA is a program called `bluealsa`. By default, this program shall be
run as a root during system startup. It will register `org.bluealsa` service in the D-Bus system
bus, which can be used for accessing configured audio devices. In general, BlueALSA acts as a
proxy between BlueZ and ALSA.

In order to stream audio to the e.g. Bluetooth headset, firstly one has to connect the device. The
most straightforward method is to use BlueZ CLI utility called `bluetoothctl`. When the device is
connected one can use the `bluealsa` virtual PCM device as follows:

	$ aplay -D bluealsa:SRV=org.bluealsa,DEV=XX:XX:XX:XX:XX:XX,PROFILE=a2dp Bourree_in_E_minor.wav

Setup parameters of the bluealsa PCM device can be set in the local `.asoundrc` configuration file
like this:

	$ cat ~/.asoundrc
	defaults.bluealsa.service "org.bluealsa"
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

In addition to A2DP profile, used for high quality audio, BlueALSA also allows to use phone audio
connection via SCO link. One can use either build-in HSP/HFP support, which implements only audio
related part of the specification, or use [oFono](https://01.org/ofono) service as a back-end. In
order to open SCO audio connection one shall switch to `sco` profile like follows:

	$ aplay -D bluealsa:SRV=org.bluealsa,DEV=XX:XX:XX:XX:XX:XX,PROFILE=sco Bourree_in_E_minor.wav

The list of available BlueALSA PCMs (provided by connected Bluetooth devices with audio
capabilities) can be obtained directly from [BlueALSA D-Bus API](doc/bluealsa-api.txt) or using
`bluealsa-aplay` as a convenient wrapper as follows:

	$ bluealsa-aplay -L

In order to control input or output audio level, one can use provided `bluealsa` control plugin.
This plugin allows adjusting the volume of the audio stream or simply mute/unmute it, e.g.:

	$ amixer -D bluealsa sset '<control name>' 70%

where the control name is the name of a connected Bluetooth device with a control element suffix,
e.g.:

	$ amixer -D bluealsa sset 'Jabra MOVE v2.3.0 - A2DP' 50%

For more advanced ALSA configuration, consult the [asoundrc on-line
documentation](https://www.alsa-project.org/main/index.php/Asoundrc) provided by the AlsaProject
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
	course, if one is compiling alsa-lib from source), or if it is not possible (installation from a
	package repository), disable it via an environmental variable, as follows: `export
	LIBASOUND_THREAD_SAFE=0`. Just take a look at involved
	[hacks](http://git.alsa-project.org/?p=alsa-lib.git;a=blob;f=src/pcm/pcm_ioplug.c;h=1dc198e7c99c933264fa25c9d7dbac5153bf0860;hb=1bf144013cffdeb41a5df3a11a8eb2596c5ea2b5#l682)
	(search for "to avoid deadlock" comments) and decide for yourself.

3. Couldn't acquire D-Bus name: org.bluealsa

	It is not possible to run more than one instance of the BlueALSA server per D-Bus interface. If
	one tries to run second instance, it will fail with the `"Couldn't acquire D-Bus name:
	org.bluealsa"` error message. This message might also appear when D-Bus policy does not allow
	acquiring "org.bluealsa" name for a particular user - by default only root is allowed to start
	BlueALSA server.


Resources
---------

1. [Bluetooth Adopted Specifications](https://www.bluetooth.com/specifications/adopted-specifications)
2. [Bluetooth Design Guidelines](https://developer.apple.com/hardwaredrivers/BluetoothDesignGuidelines.pdf)
3. [RTP Payload Format for MPEG-4](https://tools.ietf.org/html/rfc6416)
4. [Coding of MPEG-4 Audio](https://www.iso.org/standard/42739.html)
5. [ALSA project library reference](https://www.alsa-project.org/alsa-doc/alsa-lib/index.html)
