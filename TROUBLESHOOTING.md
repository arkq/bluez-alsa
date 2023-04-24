# Troubleshooting BlueALSA

Solutions to some of the most commonly encountered errors when using BlueALSA.

## Couldn't acquire D-Bus name: org.bluealsa

The BlueALSA server registers a unique "well-known service name" with D-Bus,
which is used by client applications to identify the correct service instance.
By default it uses the name "org.bluealsa". There are three reasons why
starting the service may fail with the
`"Couldn't acquire D-Bus name: org.bluealsa"` error message:

- The BlueALSA D-Bus policy file is not installed, or is in the wrong
location.\
In a default install, the file should be
`/etc/dbus-1/system.d/bluealsa.conf`. Check with your distribution
documentation in case D-Bus uses a different location on your system.
Re-install BlueALSA if the file is missing.

- The user account that the BlueALSA service is started under is not
permitted to do so by the D-Bus policy.\
The BlueALSA D-Bus policy file must contain a rule that permits the BlueALSA
service account to register names with the prefix `org.bluealsa`. The default
BlueALSA D-Bus policy file permits only `root` to register the prefix
`org.bluealsa`. To permit some other user account the D-Bus policy must be
updated. For example to permit the BlueALSA service to run under the account
name `bluealsa` in addition to being able to run as `root`:

   ```xml
   <busconfig>

     <policy user="bluealsa">
       <allow own_prefix="org.bluealsa"/>
       <allow send_destination="org.bluealsa"/>
     </policy>

     <policy user="root">
       <allow own_prefix="org.bluealsa"/>
       <allow send_destination="org.bluealsa"/>
     </policy>

     <policy group="audio">
       <allow send_destination="org.bluealsa"/>
     </policy>

   </busconfig>
   ```

- Another instance of the BlueALSA service is already running.\
To run a second instance of the BlueALSA service, it must use a different
well-known service name. This will also require updating the BlueALSA D-Bus
policy file. See the manual page [bluealsa(8)][] for more information and an
example of running multiple `bluealsa` instances.

If the D-Bus policy file is edited, then it is necessary to refresh the D-Bus
service for the change to take effect. On most systems this can be achieved
with (as `root`) :

```sh
systemctl reload dbus.service
```

[bluealsa(8)]: doc/bluealsa.8.rst

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
grant permission only to members of the `audio` group and `root` (this is in
line with normal practice on ALSA systems whereby membership of the `audio`
group is required to use sound card devices).

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

## Using BlueALSA alongside PulseAudio or PipeWire

It is not advisable to run BlueALSA if either PulseAudio or PipeWire are also
running with their own Bluetooth modules enabled. If one would like to have a
deterministic setup, it is first necessary to disable Bluetooth in those
applications.

On startup, the BlueALSA service will issue warnings if some other application
has already registered the Bluetooth Audio profiles:

```text
bluealsa: W: UUID already registered in BlueZ [hci0]: 0000110A-0000-1000-8000-00805F9B34FB
bluealsa: W: UUID already registered in BlueZ [hci0]: 0000110B-0000-1000-8000-00805F9B34FB
bluealsa: W: UUID already registered in BlueZ: 0000111F-0000-1000-8000-00805F9B34FB
```

However, as it is normal practice to start BlueALSA at boot and to start
PulseAudio only when the user logs in, these warnings may not appear in the
logs.

In the unlikely event that one should need to run BlueALSA at the same time as
PulseAudio, there are some hints on how to disable the PulseAudio Bluetooth
modules in the wiki: [PulseAudio integration][]

[PulseAudio integration]: https://github.com/arkq/bluez-alsa/wiki/PulseAudio-integration

## ALSA thread-safe API (alsa-lib >= 1.1.2, <= 1.1.3)

ALSA library versions 1.1.2 and 1.1.3 had a bug in their thread-safe API
functions. This bug does not affect hardware audio devices, but it affects
many software plug-ins. Random deadlocks are inevitable. The best advice is
to use a more recent alsa-lib release, or if that is not possible then
disable the thread locking code via an environment variable, as follows:
`export LIBASOUND_THREAD_SAFE=0`.
