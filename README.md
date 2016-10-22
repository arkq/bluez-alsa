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
	$ make && make install


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

In order to control input or output audio level, one can use provided `bluealsa` control plugin.
This plugin allows adjusting the volume of the audio stream or simply mute/unmute it, e.g.:

	$ amixer -D bluealsa '<device name>' sset 70%

where the device name is the name of connected Bluetooth device.


Resources
---------

1. [Bluetooth Adopted Specifications](https://www.bluetooth.com/specifications/adopted-specifications)
2. [Bluetooth Design Guidelines](https://developer.apple.com/hardwaredrivers/BluetoothDesignGuidelines.pdf)
3. [RTP Payload Format for MPEG-4](https://tools.ietf.org/html/rfc3016)
4. [Coding of MPEG-4 Audio](http://www.iso.org/iso/iso_catalogue/catalogue_tc/catalogue_detail.htm?csnumber=42739)
