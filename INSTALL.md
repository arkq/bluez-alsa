# BlueALSA Installation

Given its aim of small size and minimum redundancy, BlueALSA makes many of its
features optional and only includes them when explicitly requested when
configuring the build. The number of options is therefore large, too large to
be covered fully here. For a comprehensive installation guide, please look at
the [Installation from source][] project wiki page. If you've found something
missing or incorrect, feel free to make a wiki contribution.

[Installation from source]: https://github.com/arkq/bluez-alsa/wiki/Installation-from-source

## Configuration

Firstly, create the `configure` script. Run, in the top level project
directory:

```sh
autoreconf --install
```

then, to see a complete list of all options:

```sh
./configure --help
```

Dependencies:

- [alsa-lib](https://www.alsa-project.org/) >= 1.0.27
- [bluez](http://www.bluez.org/) >= 5.51
- [glib](https://wiki.gnome.org/Projects/GLib) >= 2.58.2 with GIO support
- [sbc](https://git.kernel.org/cgit/bluetooth/sbc.git) >= 1.5
- [docutils](https://docutils.sourceforge.io) (when man pages build is enabled
  with `--enable-manpages`)
- [fdk-aac](https://github.com/mstorsjo/fdk-aac) (when AAC support is enabled
  with `--enable-aac`)
- [lc3](https://github.com/google/liblc3) >= 1.0.0 (when LC3-SWB support is
  enabled with `--enable-lc3-swb`)
- [lc3plus](https://www.iis.fraunhofer.de/en/ff/amm/communication/lc3.html)
  (when LC3plus support is enabled with `--enable-lc3plus`)
- [libldac](https://github.com/EHfive/ldacBT) (when LDAC support is enabled
  with `--enable-ldac`)
- [libopenaptx](https://github.com/pali/libopenaptx) (when apt-X support is
  enabled and `--with-libopenaptx` is used)
- [mp3lame](https://lame.sourceforge.net/) (when MP3 support is enabled with
  `--enable-mp3lame`)
- [mpg123](https://www.mpg123.org/) (when MPEG decoding support is enabled with
  `--enable-mpg123`)
- [openaptx](https://github.com/arkq/openaptx) (when apt-X support is enabled
  with `--enable-aptx` and/or `--enable-aptx-hd`)
- [opus](https://opus-codec.org/) (when Opus support is enabled with
  `--enable-opus`)
- [spandsp](https://www.soft-switch.org) (when mSBC support is enabled with
  `--enable-msbc`)

Dependencies for client applications (e.g. `bluealsactl` or `bluealsa-aplay`):

- [libdbus](https://www.freedesktop.org/wiki/Software/dbus/)

Dependencies for `bluealsa-rfcomm` (when `--enable-rfcomm` is specified during
configuration):

- [readline](https://tiswww.case.edu/php/chet/readline/rltop.html)

Dependencies for `hcitop` (when `--enable-hcitop` is specified during
configuration):

- [libbsd](https://libbsd.freedesktop.org/)
- [ncurses](https://www.gnu.org/software/ncurses/)

If it is intended to use BlueALSA on a system that uses `systemd`, then it is
recommended to include the option `--enable-systemd` as this will create
service unit files.
See the [systemd integration][] wiki page for more information.

[systemd integration]: https://github.com/arkq/bluez-alsa/wiki/Systemd-integration

If intending to run the `bluealsad` daemon as a non-root user then it is
recommended to use the `--with-bluealsaduser=USER` option as this will configure
the BlueALSA D-Bus policy file with correct permissions for that user account,
and also include that user in the systemd service unit file when used in
combination with `--enable-systemd`.

If not using systemd, then some manual setup of the host will be required, see
[Runtime Environment](#runtime-environment) below.

Once the desired options have been chosen, run:

```sh
mkdir build && cd build
../configure [ OPTION ... ]
```

## Build

When the project is configured, compile it by running in the build directory:

```shell
make
```

When building from the git sources, if `git pull` is used to update the source
tree, then it is recommended to refresh the build in order to update the
version identifier embedded in the configure files. In the top-level directory
run:

```shell
autoreconf --install --force
```

then in the build directory run `make clean` before running `make`.

## Installation

The built components can be installed on the local system with

```shell
sudo make install
```

To install into a directory that can be packaged and copied to other hosts (for
example a sub-directory called BLUEALSA in the current directory):

```shell
sudo make DESTDIR=$(pwd)/BLUEALSA install
```

## Runtime Environment

### Storage directory

When using `systemd`, all the necessary files and directories are created by
the `bluealsa.service` unit at runtime. If not using `systemd`, or if the
 `--enable-systemd` option was not used during configuration, then it is
necessary to manually create the directory used by BlueALSA for persistent
state storage. This directory should be called `bluealsa` and be located under
the system local state directory, which is normally `/var/lib`. The directory
owner must be the user account that the `bluealsad` daemon is run under, and
to prevent accidental corruption of the state files the permissions should be
`rwx------`. For example, on a standard file hierarchy, with the `bluealsad`
daemon running as user `bluealsa`:

```sh
sudo mkdir /var/lib/bluealsa
sudo chown bluealsa /var/lib/bluealsa
sudo chmod 0700 /var/lib/bluealsa
```

### User accounts

The BlueALSA installation does not create any user accounts.

### D-Bus policy

A D-Bus policy file is required to enable the `bluealsad` daemon to register
with D-Bus as a service. The default policy file created by the BlueALSA
installation enables `root` to register the service `org.bluealsa` and enables
members of the group `audio` to use BlueALSA PCMs and the BlueALSA mixer. If
the option `--with-bluealsaduser=USER` was used when configuring then the
policy file enables user USER instead of `root` to register the `org.bluealsa`
service. If that option was not used, then it is necessary to edit the policy
file to grant permission to a non-root user. The policy file is located at

`/usr/share/dbus-1/system.d/org.bluealsa.conf`.

For example:

```xml
<!-- This configuration file specifies the required security policies
     for BlueALSA core daemon to work. -->

<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>

  <!-- ../system.conf have denied everything, so we just punch some holes -->

  <policy user="bluealsa">
    <allow own_prefix="org.bluealsa"/>
    <allow send_destination="org.bluealsa"/>
  </policy>

  <policy group="audio">
    <allow send_destination="org.bluealsa"/>
  </policy>

</busconfig>
```
