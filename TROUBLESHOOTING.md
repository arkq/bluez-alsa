# Troubleshooting BlueALSA

Solutions to some of the most commonly encountered errors when using BlueALSA.

## Using BlueALSA alongside PulseAudio or PipeWire

Due to BlueZ limitations, only one program can register as provider of
Bluetooth audio profile implementation. So it is not possible to use
BlueALSA if either PulseAudio or PipeWire are also running with their own
Bluetooth modules enabled; it is first necessary to disable Bluetooth in
those applications.

On startup, the bluealsa service will issue warnings if some other application
has already registered the Bluetooth Audio profiles:

```text
bluealsa: W: UUID already registered in BlueZ [hci0]: 0000110A-0000-1000-8000-00805F9B34FB
bluealsa: W: UUID already registered in BlueZ [hci0]: 0000110B-0000-1000-8000-00805F9B34FB
bluealsa: W: UUID already registered in BlueZ: 0000111F-0000-1000-8000-00805F9B34FB
```

## Couldn't acquire D-Bus name: org.bluealsa

It is not possible to run more than one instance of the BlueALSA server per
D-Bus interface. If one tries to run second instance, it will fail with the
`"Couldn't acquire D-Bus name: org.bluealsa"` error message. This message
might also appear when D-Bus policy does not allow acquiring "org.bluealsa"
name for a particular user - by default only root is allowed to start
BlueALSA server.

## Couldn't get BlueALSA PCM: PCM not found

In contrast to standard ALSA sound cards, BlueALSA does not expose all PCMs
right away. In the first place it is required to connect remote Bluetooth
device with desired Bluetooth profile - run `bluealsa --help` for the list
of available profiles. For querying currently connected audio profiles (and
connected devices), run `bluealsa-aplay --list-devices`. The common
misconception is an attempt to use A2DP playback device as a capture one in
case where A2DP is not listed in the "List of CAPTURE Bluetooth Devices"
section.

Additionally, the cause of the "PCM not found" error might be an incorrect
ALSA PCM name. Run `bluealsa-aplay --list-pcms` for the list of currently
available ALSA PCM names - it might give you a hint what is wrong with your
`.asoundrc` entry. Also, take a look at the [bluealsa-plugins manual
page](doc/bluealsa-plugins.7.rst).

## Couldn't get BlueALSA PCM: Rejected send message

This error message indicates that the user does not have permission to use
the BlueALSA service. BlueALSA client applications require permission from
D-Bus to communicate with the BlueALSA service. This permission is granted
by a D-Bus policy configuration file. A default BlueALSA installation will
grant permission only to members of the `audio` group and `root`.

There are several reasons why this permission is not granted:

- The D-Bus service has not been refreshed after installing BlueALSA.\
Try `sudo systemctl reload dbus.service`. If that does not work, try
rebooting.

- The user is not a member of the `audio` group.

- The user session was created before the user was added to the `audio`
 group.\
Log out, then log in again.

- The BlueALSA D-Bus policy file is not installed, or is in the wrong
 location.\
In a default install, the file should be
`/etc/dbus-1/system.d/bluealsa.conf`. Check with your distribution
documentation in case D-Bus uses a different location on your system.
Re-install BlueALSA if the file is missing.

## ALSA thread-safe API (alsa-lib >= 1.1.2, <= 1.1.3)

ALSA library versions 1.1.2 and 1.1.3 had a bug in their thread-safe API
functions. This bug does not affect hardware audio devices, but it affects
many software plug-ins. Random deadlocks are inevitable. The best advice is
to use a more recent alsa-lib release, or if that is not possible then
disable the thread locking code via an environment variable, as follows:
`export LIBASOUND_THREAD_SAFE=0`.
